import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SUBstation

// Preferences › Plug-ins (Session.plugins, the old PluginsPage): where VST3
// plug-ins are looked for (the standard folders, dimmed and always searched;
// the user's own, red if they don't exist any more) and rescanning them.
// Folders added or removed apply at once: only the new files are read, and a
// removed folder's plug-ins leave the browser. The scan's status shows the
// count and the files that could not be read (why, in its tooltip).
ColumnLayout {
    id: page

    readonly property var plugins: Session.plugins
    readonly property alias folderList: folders

    // Adds a folder (and scans it); the folder (added, or listed already) is current.
    function addFolder(folder) {
        if (!folder)
            return
        plugins.addFolder(folder)
        folders.currentIndex = plugins.folders.find(folder)
    }

    // Takes the current folder off the list (and scans again, so its plug-ins go).
    function removeFolder() {
        const row = folders.currentItem
        if (!row || !row.removable)
            return
        plugins.removeFolder(row.path)
        folders.currentIndex = -1
    }

    spacing: 6

    Label { text: qsTr("VST3 Folders") }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        implicitHeight: 160
        color: Theme.panel

        ListView {
            id: folders
            objectName: "pluginFolders"
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            currentIndex: -1
            model: page.plugins.folders
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: folder

                required property int index
                required property string path
                required property var model
                required property bool standard
                required property bool exists
                required property bool removable
                required property string toolTip

                width: ListView.view.width
                text: model.display
                highlighted: ListView.isCurrentItem
                onClicked: folders.currentIndex = index

                contentItem: Text {
                    text: folder.text
                    font: folder.font
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                    color: folder.highlighted ? Theme.accentText
                                              : folder.standard ? Theme.textDim
                                                                : (folder.exists ? Theme.text : "#ff6b5e")
                }

                ToolTip.visible: hovered && toolTip !== ""
                ToolTip.text: toolTip
                ToolTip.delay: 700
            }
        }
    }

    RowLayout {
        spacing: 6

        RoleButton {
            objectName: "addPluginFolder"
            text: qsTr("Add Folder…")
            onClicked: folderDialog.open()
        }
        RoleButton {
            objectName: "removePluginFolder"
            text: qsTr("Remove")
            enabled: folders.currentItem !== null && folders.currentItem.removable
            onClicked: page.removeFolder()
        }
        Item {
            Layout.fillWidth: true
        }
    }

    Item {
        implicitHeight: 2
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        RoleButton {
            objectName: "rescanPlugins"
            text: qsTr("Rescan Plug-ins")
            tooltip: qsTr("Read every plug-in file again, also those that could not be read before.")
            enabled: !page.plugins.scanning
            onClicked: page.plugins.rescan()
        }
        Label {
            id: scanStatus
            objectName: "scanStatus"
            Layout.fillWidth: true
            text: page.plugins.statusText
            wrapMode: Text.Wrap
            color: Theme.textDim

            HoverHandler {
                id: statusHover
            }
            ToolTip.visible: statusHover.hovered && !page.plugins.scanning && page.plugins.failuresText !== ""
            ToolTip.text: page.plugins.failuresText
            ToolTip.delay: 700
        }
    }

    FolderDialog {
        id: folderDialog
        objectName: "pluginFolderDialog"
        title: qsTr("Add VST3 Folder")
        currentFolder: FileUrls.homeUrl()
        onAccepted: page.addFolder(FileUrls.localPath(selectedFolder))
    }
}
