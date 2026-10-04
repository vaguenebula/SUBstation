import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// The main window, laid out as the old MainWindow: the transport bar on top;
// below it a horizontal split of the browser (left) and, on the right, a
// vertical split of the arrangement over the device view; the status bar at
// the bottom. The views are placeholders until they are written; every menu
// action is here with its shortcut, its handler a stub (notWired) until the
// app layer is connected.
ApplicationWindow {
    id: window

    // MainWindow.show_message: a status line for 8 seconds.
    function showMessage(text) {
        statusText.text = text
        messageTimer.restart()
    }

    // MainWindow._show_plugins_loading: how far a project's plug-ins have
    // loaded, at the right of the status bar (gone once they all have).
    function showPluginsLoading(loaded, total) {
        pluginsLoading.visible = total > 0
        pluginsLabel.text = qsTr("Loading plug-ins: %1 of %2").arg(loaded).arg(total)
        pluginsBar.to = Math.max(1, total)
        pluginsBar.value = loaded
    }

    function notWired(actionText) {
        showMessage(qsTr("%1: not connected yet").arg(Theme.withoutMnemonics(actionText)))
    }

    width: 1440
    height: 860
    visible: true
    title: qsTr("Untitled - SUBstation")

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action { objectName: "newProject"; text: qsTr("&New Project"); shortcut: StandardKey.New; onTriggered: window.notWired(text) }
            Action { objectName: "openProject"; text: qsTr("&Open…"); shortcut: StandardKey.Open; onTriggered: window.notWired(text) }
            Menu {
                id: recentMenu
                objectName: "recentMenu"
                title: qsTr("Open &Recent")
                // Filled from files/recent (at most 10) when it opens: "&1  name", ...,
                // a separator and "&Clear List"; with none, this line only.
                MenuItem { text: qsTr("No Recent Projects"); enabled: false }
            }
            MenuSeparator {}
            Action { objectName: "save"; text: qsTr("&Save"); shortcut: StandardKey.Save; onTriggered: window.notWired(text) }
            Action { objectName: "saveAs"; text: qsTr("Save &As…"); shortcut: "Ctrl+Shift+S"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "exportAudio"; text: qsTr("&Export Audio…"); shortcut: "Ctrl+Shift+R"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "quit"; text: qsTr("&Quit"); shortcut: "Ctrl+Q"; onTriggered: window.close() }
        }
        Menu {
            title: qsTr("&Edit")
            Action { id: undoAction; objectName: "undo"; text: qsTr("&Undo"); shortcut: StandardKey.Undo; onTriggered: window.notWired(text) }
            Action { id: redoAction; objectName: "redo"; text: qsTr("&Redo"); shortcut: "Ctrl+Y"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "cut"; text: qsTr("Cu&t"); shortcut: StandardKey.Cut; onTriggered: window.notWired(text) }
            Action { objectName: "copy"; text: qsTr("&Copy"); shortcut: StandardKey.Copy; onTriggered: window.notWired(text) }
            Action { objectName: "paste"; text: qsTr("&Paste"); shortcut: StandardKey.Paste; onTriggered: window.notWired(text) }
            Action { objectName: "duplicate"; text: qsTr("D&uplicate"); shortcut: "Ctrl+D"; onTriggered: window.notWired(text) }
            Action { objectName: "rename"; text: qsTr("&Rename"); shortcut: "Ctrl+R"; onTriggered: window.notWired(text) }
            Action { objectName: "split"; text: qsTr("&Split"); shortcut: "Ctrl+E"; onTriggered: window.notWired(text) }
            Action { objectName: "consolidate"; text: qsTr("C&onsolidate"); shortcut: "Ctrl+J"; onTriggered: window.notWired(text) }
            Action { objectName: "reverseClips"; text: qsTr("Re&verse Clips"); shortcut: "R"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "freeze"; text: qsTr("&Freeze / Unfreeze Track"); shortcut: "Ctrl+Shift+F"; onTriggered: window.notWired(text) }
            Action { objectName: "flatten"; text: qsTr("Flatten Track"); onTriggered: window.notWired(text) }
            Action { id: deleteAction; objectName: "delete"; text: qsTr("&Delete"); shortcut: StandardKey.Delete; onTriggered: window.notWired(text) }
            Action { objectName: "selectAll"; text: qsTr("Select &All"); shortcut: StandardKey.SelectAll; onTriggered: window.notWired(text) }
            MenuSeparator {}
            // Enabled while automation is overridden (the bridge's has_overrides).
            Action { objectName: "reEnableAutomation"; text: qsTr("Re-Enable Automation"); enabled: false; onTriggered: window.notWired(text) }
            Action { objectName: "soloSelectedTracks"; text: qsTr("Solo Selected Tracks"); shortcut: "S"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "playStop"; text: qsTr("Play / Stop"); shortcut: "Space"; onTriggered: window.notWired(text) }
            Action { objectName: "record"; text: qsTr("Record"); shortcut: "F9"; onTriggered: window.notWired(text) }
            Menu {
                objectName: "recordQuantization"
                title: qsTr("Record &Quantization")
                // Where recorded MIDI notes start: as played, or on this grid
                // (record/quantize in the preferences; checked from it).
                ActionGroup { id: quantizeGroup }
                Action { text: qsTr("No Quantization"); checkable: true; checked: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: "1/4"; checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: "1/8"; checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: qsTr("1/8 Triplet"); checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: "1/16"; checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: qsTr("1/16 Triplet"); checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
                Action { text: "1/32"; checkable: true; ActionGroup.group: quantizeGroup; onTriggered: window.notWired(text) }
            }
            Action { objectName: "goToStart"; text: qsTr("Go to Start"); shortcut: "Home"; onTriggered: window.notWired(text) }
            Action { objectName: "loop"; text: qsTr("Loop"); shortcut: "Ctrl+L"; checkable: true; onTriggered: window.notWired(text) }
            Action { objectName: "findInBrowser"; text: qsTr("Find in Browser"); shortcut: "Ctrl+F"; onTriggered: window.notWired(text) }
        }
        Menu {
            title: qsTr("&Create")
            Action { objectName: "insertAudioTrack"; text: qsTr("Insert Audio &Track"); shortcut: "Ctrl+T"; onTriggered: window.notWired(text) }
            Action { objectName: "insertMidiTrack"; text: qsTr("Insert &MIDI Track"); shortcut: "Ctrl+Shift+T"; onTriggered: window.notWired(text) }
            Action { objectName: "insertReturnTrack"; text: qsTr("Insert &Return Track"); shortcut: "Ctrl+Alt+T"; onTriggered: window.notWired(text) }
            Action { id: insertMidiClipAction; objectName: "insertMidiClip"; text: qsTr("Insert MIDI &Clip"); shortcut: "Ctrl+Shift+D"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            // (Devices, in the device view.)
            Action { objectName: "groupTracks"; text: qsTr("&Group Tracks"); shortcut: "Ctrl+G"; onTriggered: window.notWired(text) }
            Action { objectName: "ungroupTracks"; text: qsTr("&Ungroup Tracks"); shortcut: "Ctrl+Shift+G"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "deleteSelectedTracks"; text: qsTr("Delete Selected Tracks"); onTriggered: window.notWired(text) }
        }
        Menu {
            title: qsTr("&View")
            Action { objectName: "browser"; text: qsTr("&Browser"); shortcut: "Ctrl+Alt+B"; checkable: true; checked: true; onToggled: browser.visible = checked }
            Action { objectName: "deviceView"; text: qsTr("&Device View"); shortcut: "Ctrl+Alt+L"; checkable: true; checked: true; onToggled: devicePanel.visible = checked }
            Action { objectName: "clipView"; text: qsTr("&Clip View"); shortcut: "Shift+Tab"; onTriggered: window.notWired(text) }
            Action { objectName: "automation"; text: qsTr("&Automation"); shortcut: "A"; onTriggered: window.notWired(text) }
            // Plug-in editors are Win32 windows: closing the foremost is for Windows only.
            MenuItem {
                visible: Qt.platform.os === "windows"
                height: visible ? implicitHeight : 0
                action: Action { objectName: "closePluginEditor"; text: qsTr("Close Plug-in &Editor"); shortcut: "Ctrl+W"; enabled: Qt.platform.os === "windows"; onTriggered: window.notWired(text) }
            }
            MenuSeparator {}
            Action { id: zoomInAction; objectName: "zoomIn"; text: qsTr("Zoom &In"); shortcut: "+"; onTriggered: window.notWired(text) }
            Action { id: zoomOutAction; objectName: "zoomOut"; text: qsTr("Zoom &Out"); shortcut: "-"; onTriggered: window.notWired(text) }
            Action { objectName: "zoomToArrangement"; text: qsTr("Zoom to &Arrangement"); shortcut: "Z"; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "narrowGrid"; text: qsTr("Narrow Grid"); shortcut: "Ctrl+1"; onTriggered: window.notWired(text) }
            Action { objectName: "widenGrid"; text: qsTr("Widen Grid"); shortcut: "Ctrl+2"; onTriggered: window.notWired(text) }
            Action { objectName: "snapToGrid"; text: qsTr("Snap to Grid"); shortcut: "Ctrl+4"; checkable: true; checked: true; onTriggered: window.notWired(text) }
        }
        Menu {
            title: qsTr("&Options")
            Action { objectName: "preferences"; text: qsTr("&Preferences…"); shortcut: "Ctrl+,"; onTriggered: window.notWired(text) }
            Action { objectName: "rescanPlugins"; text: qsTr("&Rescan Plug-ins"); onTriggered: window.notWired(text) }
            Action { objectName: "computerMidiKeyboard"; text: qsTr("Computer &MIDI Keyboard"); shortcut: "M"; checkable: true; onTriggered: window.notWired(text) }
            MenuSeparator {}
            Action { objectName: "lockEnvelopes"; text: qsTr("&Lock Envelopes"); checkable: true; onTriggered: window.notWired(text) }
        }
        Menu {
            title: qsTr("&Help")
            Action { objectName: "about"; text: qsTr("&About SUBstation"); onTriggered: window.notWired(text) }
        }
    }

    // The actions' other shortcuts (an Action has one).
    Shortcut { sequences: ["Ctrl+Shift+Z"]; onActivated: redoAction.trigger() }
    Shortcut { sequences: ["Backspace"]; onActivated: deleteAction.trigger() }
    Shortcut { sequences: ["Ctrl+Shift+M"]; onActivated: insertMidiClipAction.trigger() }
    Shortcut { sequences: ["=", StandardKey.ZoomIn]; onActivated: zoomInAction.trigger() }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: zoomOutAction.trigger() }

    header: Placeholder {
        id: transportBar
        objectName: "transportBar"
        implicitHeight: 40
        color: Theme.window
        label: qsTr("Transport Bar")
        detail: qsTr("tempo, time signature, metronome, key | position, play, stop, record, count-in | scope | loop, follow, CPU, device")
    }

    SplitView {
        id: splitter
        anchors.fill: parent
        orientation: Qt.Horizontal

        Placeholder {
            id: browser
            objectName: "browser"
            SplitView.preferredWidth: 300
            SplitView.minimumWidth: 120
            label: qsTr("Browser")
        }

        SplitView {
            id: right
            SplitView.fillWidth: true
            orientation: Qt.Vertical

            Placeholder {
                id: arrangement
                objectName: "arrangement"
                SplitView.fillHeight: true
                SplitView.minimumHeight: 120
                color: Theme.emptyArea
                label: qsTr("Arrangement")
                detail: qsTr("ruler, track lanes and headers, returns, master")
            }
            Placeholder {
                id: devicePanel
                objectName: "devicePanel"
                SplitView.preferredHeight: 280
                label: qsTr("Device View")
                detail: qsTr("the selected track's devices; the clip view; the piano roll")
            }
        }
    }

    // QStatusBar: PANEL with a BORDER line above, dim text.
    footer: Rectangle {
        implicitHeight: 22
        color: Theme.panel

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.border
        }

        Text {
            id: statusText
            objectName: "statusText"
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.right: pluginsLoading.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Ready")
            color: Theme.textDim
            font: Theme.font
            elide: Text.ElideRight
        }

        Timer {
            id: messageTimer
            interval: 8000
            onTriggered: statusText.text = ""
        }

        RowLayout {
            id: pluginsLoading
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: false
            spacing: 6

            Text {
                id: pluginsLabel
                color: Theme.textDim
                font: Theme.font
            }
            ProgressBar {
                id: pluginsBar
                Layout.preferredWidth: 120
                Layout.preferredHeight: 10
            }
        }
    }
}
