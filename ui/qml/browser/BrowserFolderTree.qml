import QtQuick
import QtQuick.Controls
import SUBstation

// A place's folder tree, shown while its entry is chosen and nothing is
// searched (the old QTreeView over a QFileSystemModel of the audio files,
// flattened here by FolderTreeModel). A folder opens and closes with its
// arrow, a double-click, or Right and Left (which also go to its first entry
// and to the folder it is in); selecting an audio file previews it;
// Return/Enter or a double-click on a file adds it.
SelectionList {
    id: tree

    readonly property var browser: Session.browser
    readonly property alias folders: folderModel
    // The browser's folder model is made when it is first wanted.
    property bool wanted: false

    objectName: "folderTree"
    model: FolderTreeModel {
        id: folderModel
        sourceModel: tree.wanted ? tree.browser.folderModel : null
        rootIndex: tree.browser.treeRootIndex
        onRootIndexChanged: tree.resetSelection(-1)
        onModelReset: tree.resetSelection(-1)
    }

    onCurrentIndexChanged: if (currentIndex >= 0) browser.treeCurrentChanged(folderModel.path(currentIndex))
    onActivated: (row, byKey) => {
        if (!folderModel.isDir(row))
            browser.activateFile(folderModel.path(row))
        else if (!byKey)
            folderModel.toggle(row)  // (a double-click on a folder opens or closes it)
    }

    keyHandler: event => {
        const row = currentIndex
        if (row < 0 || (event.key !== Qt.Key_Right && event.key !== Qt.Key_Left))
            return false
        if (event.key === Qt.Key_Right) {
            if (folderModel.isDir(row) && !folderModel.isExpanded(row))
                folderModel.expand(row)
            else if (folderModel.isExpanded(row) && row + 1 < count && folderModel.depth(row + 1) > folderModel.depth(row))
                moveCurrent(row + 1, false)
        } else if (folderModel.isExpanded(row)) {
            folderModel.collapse(row)
        } else if (folderModel.parentRow(row) >= 0) {
            moveCurrent(folderModel.parentRow(row), false)
        }
        return true
    }

    delegate: Item {
        id: entry

        required property int index
        required property string name
        required property string path
        required property int depth
        required property bool isDir
        required property bool expanded
        required property bool hasChildren

        readonly property bool selected: tree.selectedRows.indexOf(index) >= 0
        readonly property int indent: 20  // (QTreeView's indentation)

        width: ListView.view.width
        implicitHeight: Math.max(16, label.implicitHeight) + 4

        Rectangle {
            anchors.fill: parent
            color: entry.selected ? Theme.accent : (area.containsMouse ? Theme.panelAlt : "transparent")
        }

        SelectionRowArea {
            id: area
            anchors.fill: parent
            list: tree
            row: entry.index
        }

        // The folder's arrow: right while closed, down while open.
        Item {
            id: arrow
            x: entry.depth * entry.indent
            width: entry.indent
            height: parent.height

            Icon {
                anchors.centerIn: parent
                visible: entry.isDir && entry.hasChildren
                name: "fold"
                checked: !entry.expanded
                size: 9
                color: entry.selected ? Theme.accentText : Theme.textDim
            }
            MouseArea {
                anchors.fill: parent
                enabled: entry.isDir
                onClicked: folderModel.toggle(entry.index)
            }
        }
        Icon {
            id: icon
            anchors.left: arrow.right
            anchors.verticalCenter: parent.verticalCenter
            name: entry.isDir ? "folder" : "waveform"
            size: 16
        }
        Text {
            id: label
            anchors.left: icon.right
            anchors.leftMargin: 4
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: entry.name
            font: Theme.font
            elide: Text.ElideRight
            color: entry.selected ? Theme.accentText : Theme.text
        }
    }
}
