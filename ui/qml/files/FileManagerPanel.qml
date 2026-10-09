import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SUBstation

// The File Manager, on the right of the window (View › File Manager,
// Ctrl+Alt+F), as Ableton's: Session.files. It lists the files the project
// plays (its audio clips' and samplers'), the missing ones first, in red, with
// what plays each; its field filters them by name and folder.
//
// While files are missing, a box over the list says how many and searches for
// them: Search (the project's folder and the browser's places) or Search
// Folder… (a folder chosen here), in the background, with Cancel. A row's menu
// (a right-click) has Hot-Swap (the browser lists similar sounds), Replace… (any
// audio file in its place, everywhere it plays), Locate… (a missing file's),
// Select Clips, Find Similar Sounds and Show in Folder. Its button at the
// right hot-swaps it (lit while that hot swap runs: a click ends it). Dropping
// an audio file onto a row (from the browser, or the system's file browser)
// replaces that file with it; a double-click selects its clips.
//
// Showing, it checks again which files are there.
Rectangle {
    id: panel

    readonly property var files: Session.files
    readonly property var hotSwap: Session.hotSwap
    readonly property alias listView: list
    readonly property alias filterField: filter

    // Its ✕: the window hides it (View › File Manager).
    signal closeRequested()

    // Show in File Manager: the file's row, current and in view.
    function reveal(path) {
        const row = files.files.rowOf(path)
        if (row < 0)
            return false
        list.currentIndex = row
        list.positionViewAtIndex(row, ListView.Contain)
        list.forceActiveFocus()
        return true
    }

    function runAction(action, path) {
        switch (action) {
        case "hotSwap":
            files.hotSwap(path)
            break
        case "replace":
            replaceDialog.path = path
            replaceDialog.currentFolder = FileUrls.folderUrl(files.dialogFolder(path))
            replaceDialog.open()
            break
        case "locate":
            locateDialog.path = path
            locateDialog.currentFolder = FileUrls.folderUrl(files.dialogFolder(path))
            locateDialog.open()
            break
        case "selectClips":
            files.selectClips(path)
            break
        case "findSimilar":
            files.findSimilar(path)
            break
        case "showInFolder":
            files.showInFolder(path)
            break
        }
    }

    // The audio file a drop brings ("" if none).
    function droppedFile(urls) {
        for (let i = 0; i < urls.length; ++i) {
            const path = FileUrls.localPath(urls[i])
            if (files.isAudioFile(path))
                return path
        }
        return ""
    }

    objectName: "fileManager"
    color: Theme.window
    implicitWidth: 300

    onVisibleChanged: if (visible) files.refresh()

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 4
            spacing: 6

            Label {
                text: qsTr("FILE MANAGER")
                font: Theme.listHeadingFont
                color: Theme.textDim
            }
            Label {
                objectName: "fileSummary"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: panel.files.summary
                color: Theme.textDim
                elide: Text.ElideRight
            }
            RoleButton {
                objectName: "closeFileManager"
                role: "flat"
                text: "✕"
                tooltip: qsTr("Hide the File Manager (Ctrl+Alt+F)")
                onClicked: panel.closeRequested()
            }
        }

        // The missing files: how many, and the search for them.
        Rectangle {
            objectName: "missingBox"
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            visible: panel.files.missingCount > 0 || panel.files.searching
            implicitHeight: missingColumn.implicitHeight + 10
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.recordOn

            ColumnLayout {
                id: missingColumn
                anchors.fill: parent
                anchors.margins: 5
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Icon {
                        name: "missing"
                    }
                    Label {
                        objectName: "missingLabel"
                        Layout.fillWidth: true
                        text: panel.files.missingCount === 1 ? qsTr("1 file is missing")
                                                             : qsTr("%1 files are missing").arg(panel.files.missingCount)
                        color: Theme.text
                        elide: Text.ElideRight
                    }
                }
                Label {
                    objectName: "searchStatus"
                    Layout.fillWidth: true
                    visible: panel.files.searching
                    text: panel.files.searchStatus
                    color: Theme.textDim
                    elide: Text.ElideMiddle
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    RoleButton {
                        objectName: "searchMissing"
                        role: "small"
                        text: qsTr("Search")
                        enabled: !panel.files.searching && panel.files.missingCount > 0
                        tooltip: qsTr("Search\nLooks for the missing files by name in the project's folder and in the browser's places, and puts those it finds in their place.")
                        onClicked: panel.files.search()
                    }
                    RoleButton {
                        objectName: "searchFolder"
                        role: "small"
                        text: qsTr("Search Folder…")
                        enabled: !panel.files.searching && panel.files.missingCount > 0
                        tooltip: qsTr("Search Folder\nLooks for the missing files in a folder you choose (and in the browser's places).")
                        onClicked: searchFolderDialog.open()
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    RoleButton {
                        objectName: "cancelSearch"
                        role: "small"
                        text: qsTr("Cancel")
                        visible: panel.files.searching
                        onClicked: panel.files.cancelSearch()
                    }
                }
            }
        }

        TextField {
            id: filter
            objectName: "fileFilter"
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            placeholderText: qsTr("Filter files")
            text: panel.files.filter
            onTextEdited: panel.files.filter = text
            Keys.onDownPressed: {
                if (list.count > 0) {
                    list.currentIndex = Math.max(0, list.currentIndex)
                    list.forceActiveFocus()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.panel

            ListView {
                id: list
                objectName: "fileList"
                anchors.fill: parent
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: -1
                model: panel.files.files
                ScrollBar.vertical: ScrollBar {}

                // (A list made again keeps nothing current.)
                Connections {
                    target: panel.files.files
                    function onModelReset() {
                        list.currentIndex = -1
                    }
                }

                delegate: Item {
                    id: row

                    required property int index
                    required property string path
                    required property string name
                    required property string folder
                    required property string uses
                    required property bool missing
                    required property bool frozen

                    readonly property bool current: ListView.isCurrentItem
                    readonly property bool swapping: panel.hotSwap.active && panel.hotSwap.originPath === path
                    readonly property color textColor: current ? Theme.accentText : (missing ? Theme.recordOn : Theme.text)

                    width: ListView.view.width
                    implicitHeight: nameText.implicitHeight + detailText.implicitHeight + 8

                    Rectangle {
                        anchors.fill: parent
                        color: drop.containsDrag ? Theme.surfaceHover
                                                 : (row.current ? Theme.accent : (area.containsMouse ? Theme.panelAlt : "transparent"))
                        border.color: drop.containsDrag ? Theme.accent : "transparent"
                    }

                    Icon {
                        id: icon
                        anchors.left: parent.left
                        anchors.leftMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        name: row.missing ? "missing" : (row.frozen ? "snowflake" : "waveform")
                        color: row.current && !row.missing ? Theme.accentText : undefined
                        size: 16
                    }
                    Text {
                        id: nameText
                        anchors.left: icon.right
                        anchors.leftMargin: 5
                        anchors.right: swapButton.left
                        anchors.rightMargin: 4
                        y: 4
                        text: row.name
                        font: Theme.listFont
                        color: row.textColor
                        elide: Text.ElideMiddle
                    }
                    Text {
                        id: detailText
                        anchors.left: nameText.left
                        anchors.right: nameText.right
                        anchors.top: nameText.bottom
                        text: (row.missing ? qsTr("Missing · ") : "") + row.uses + "  ·  " + row.folder
                        font: Theme.smallFont
                        color: row.current ? Theme.accentText : Theme.textDim
                        elide: Text.ElideMiddle
                    }

                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: mouse => {
                            list.currentIndex = row.index
                            list.forceActiveFocus()
                            if (mouse.button === Qt.RightButton) {
                                rowMenu.path = row.path
                                rowMenu.popupWith(panel.files.actions(row.path))
                            }
                        }
                        onDoubleClicked: mouse => {
                            if (mouse.button === Qt.LeftButton)
                                panel.files.selectClips(row.path)
                        }
                    }

                    RoleButton {
                        id: swapButton
                        objectName: "hotSwapButton"
                        anchors.right: parent.right
                        anchors.rightMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 20
                        role: "device-header"
                        iconName: "hotswap"
                        iconColor: row.swapping ? Theme.accentText : (row.current ? Theme.accentText : undefined)
                        lit: row.swapping
                        tooltip: row.swapping ? qsTr("Hot-Swap\nEnds the hot swap, keeping what is swapped in.")
                                              : qsTr("Hot-Swap\nThe sample selected in the browser plays in this file's place, everywhere it plays; the browser lists the sounds most like it. Click through samples to hear them in the song; double-click one (or press Enter) to keep it.")
                        onClicked: {
                            list.currentIndex = row.index
                            if (row.swapping)
                                panel.hotSwap.stop()
                            else
                                panel.files.hotSwap(row.path)
                        }
                    }

                    // An audio file dropped here replaces this one.
                    DropArea {
                        id: drop
                        anchors.fill: parent
                        onEntered: event => event.accepted = event.hasUrls && panel.droppedFile(event.urls) !== ""
                        onDropped: event => {
                            const path = panel.droppedFile(event.urls)
                            if (path !== "" && panel.files.replace(row.path, path))
                                event.acceptProposedAction()
                        }
                    }

                    ToolTip.visible: Window.window !== null && area.containsMouse && !area.pressed
                    ToolTip.text: (row.missing ? qsTr("Missing: ") : "") + row.path + "\n" + row.uses
                                  + (row.frozen ? qsTr(" (some on frozen tracks)") : "")
                                  + qsTr("\nDrop a sample here to replace it; right-click for more.")
                    ToolTip.delay: 700
                }

                Keys.onReturnPressed: if (currentIndex >= 0) panel.files.selectClips(panel.files.files.get(currentIndex).path)
                Keys.onEnterPressed: if (currentIndex >= 0) panel.files.selectClips(panel.files.files.get(currentIndex).path)
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: panel.files.fileCount === 0 ? qsTr("The project plays no audio files yet.")
                                                  : qsTr("No files match the filter.")
                color: Theme.textDim
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 6
            text: qsTr("Drop a sample onto a file to replace it everywhere.")
            color: Theme.textDim
            font: Theme.smallFont
            wrapMode: Text.WordWrap
        }
    }

    ActionMenu {
        id: rowMenu
        objectName: "fileMenu"
        property string path
        onChosen: action => panel.runAction(action, path)
    }

    FileDialog {
        id: replaceDialog
        objectName: "replaceDialog"
        property string path
        title: qsTr("Replace %1").arg(path.split("/").pop())
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Audio Files (*.wav *.wave *.flac *.mp3)")]
        onAccepted: panel.files.replace(path, FileUrls.localPath(selectedFile))
    }

    FileDialog {
        id: locateDialog
        objectName: "locateDialog"
        property string path
        title: qsTr("Locate %1").arg(path.split("/").pop())
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Audio Files (*.wav *.wave *.flac *.mp3)")]
        onAccepted: panel.files.locate(path, FileUrls.localPath(selectedFile))
    }

    FolderDialog {
        id: searchFolderDialog
        objectName: "searchFolderDialog"
        title: qsTr("Search Folder for Missing Files")
        currentFolder: FileUrls.folderUrl(panel.files.dialogFolder(""))
        onAccepted: panel.files.searchFolder(FileUrls.localPath(selectedFolder))
    }
}
