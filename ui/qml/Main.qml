import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SUBstation

// The main window (main_window.py): the transport bar on top; below it a
// horizontal split of the browser (left) and, on the right, a vertical split of
// the arrangement over the device area (the device view, or the clip view while
// clips are open in it); the status bar at the bottom, with a project's
// plug-ins loading at its right. Every menu action and shortcut calls the
// session (the main window's logic: transport, files, the Edit and Create
// commands on what is selected, renders, preferences); this file shows what it
// says and asks the user what it needs (files, unsaved changes).
//
// The arrangement and the device view are reached only through their
// interfaces, each call guarded (a placeholder has none):
//   arrangement: zoom(factor), zoomToArrangement(), narrowGrid(), widenGrid(),
//                openClipView(), renameTrack(trackId) -> bool, focusLanes();
//                snap, follow (read/write), gridStep, gridLevel; statusMessage(text)
//   devicePanel: startChainRename(rackId, chainId) -> bool, focusDevices();
//                statusMessage(text)
ApplicationWindow {
    id: window

    // The views (for the tests, and whoever needs them).
    readonly property alias transportBar: transportBar
    readonly property alias browserPanel: browser
    readonly property alias arrangementView: arrangement
    readonly property alias devicePanelView: devicePanel
    readonly property alias clipView: clipView
    readonly property alias deviceArea: deviceArea
    readonly property alias pluginKeys: pluginKeys
    // Set once the close was confirmed (no render running, nothing unsaved or the user said so).
    property bool closeConfirmed: false
    // What runs after a Save that asks where (the unsaved-changes question's Save).
    property var afterSave: null

    // MainWindow.show_message: a status line for Session.statusTimeout ms.
    function showMessage(text) {
        statusText.text = text
        messageTimer.restart()
    }

    // --- The views' interfaces (guarded: a placeholder has none) ---------------------------

    function focusLanes() {
        if (arrangement.focusLanes)
            arrangement.focusLanes()
        else
            window.contentItem.forceActiveFocus()  // (not a view that has gone, at least)
    }

    // The grid a new MIDI clip snaps to (beats; 0: snapping is off).
    function insertGridStep() {
        return arrangement.snap === true && arrangement.gridStep !== undefined ? arrangement.gridStep : 0
    }

    function zoom(factor) {
        if (arrangement.zoom)
            arrangement.zoom(factor)
    }

    // --- Files ---------------------------------------------------------------------------------

    // Runs `then` unless the user cancels: the project has no unsaved changes,
    // or they were saved (Save) or may go (Discard).
    function confirmDiscard(then) {
        if (Session.clean) {
            then()
            return
        }
        unsavedDialog.then = then
        unsavedDialog.open()
    }

    // Ctrl+S: to the project's file; a project never saved asks where first
    // (Session.saveAsRequested). `then` runs once it was saved.
    function save(then) {
        afterSave = then || null
        const saved = Session.saveProject()  // (never saved: saveAsRequested has asked where meanwhile)
        const after = afterSave
        afterSave = null
        if (saved && after)
            after()
    }

    // Save As…: asks where (starting at "<last folder>/Untitled.gilproj"); `then` runs once saved.
    function saveAs(then) {
        saveDialog.then = then || null
        saveDialog.currentFolder = FileUrls.folderUrl(Session.suggestedSavePath())
        saveDialog.selectedFile = FileUrls.fileUrl(Session.suggestedSavePath())
        saveDialog.open()
    }

    function saveChosen(path) {
        const then = saveDialog.then
        saveDialog.then = null
        if (path !== "" && Session.saveProjectAs(path) && then)
            then()
    }

    function newProject() {
        confirmDiscard(() => Session.newProject())
    }

    // Open…: asks about unsaved changes, then which file.
    function openProject() {
        confirmDiscard(() => {
            openDialog.currentFolder = FileUrls.fileUrl(Session.lastFolder)
            openDialog.open()
        })
    }

    function openChosen(path) {
        if (path !== "")
            Session.openProject(path)
    }

    // A recent project chosen: one whose file is gone is taken off the list (with a warning).
    function openRecent(path) {
        if (Session.recentProjectAvailable(path))
            confirmDiscard(() => Session.openProject(path))
    }

    // Open Recent, as it opens: the projects ("&1  name"), a separator and Clear List;
    // with none, "No Recent Projects".
    function fillRecentMenu() {
        while (recentMenu.count > 0) {
            const item = recentMenu.takeItem(0)
            if (item)
                item.destroy()
        }
        const items = Session.recentMenuItems()
        if (items.length === 0) {
            recentMenu.addItem(recentEntry.createObject(recentMenu.contentItem, {text: qsTr("No Recent Projects"), enabled: false}))
            return
        }
        for (const entry of items)
            recentMenu.addItem(recentEntry.createObject(recentMenu.contentItem, {text: entry.label, path: entry.path, tip: entry.toolTip}))
        recentMenu.addItem(menuSeparator.createObject(recentMenu.contentItem))
        recentMenu.addItem(recentEntry.createObject(recentMenu.contentItem, {text: qsTr("&Clear List"), clearList: true}))
    }

    // File › Export Audio…: the range and bit depth, then where (or why there is nothing to export).
    function exportChosen(range, bitDepth) {
        const problem = Session.exportProblem(range)
        if (problem !== "") {
            messages.information(problem)
            return
        }
        exportFileDialog.range = range
        exportFileDialog.bitDepth = bitDepth
        exportFileDialog.currentFolder = FileUrls.folderUrl(Session.suggestedExportPath())
        exportFileDialog.selectedFile = FileUrls.fileUrl(Session.suggestedExportPath())
        exportFileDialog.open()
    }

    function exportFileChosen(path) {
        if (path !== "")
            Session.exportAudio(path, exportFileDialog.range, exportFileDialog.bitDepth)
    }

    // --- Edit ----------------------------------------------------------------------------------

    // Ctrl+R: the preset current in the browser's list (if the list has the
    // focus), the rack chain last clicked (device view), or the track last
    // clicked, renamed where it shows.
    function rename() {
        const target = Session.renameTarget(browser.listFocused)
        let started = false
        if (target.kind === "preset")
            started = browser.startRename(target.path)
        else if (target.kind === "chain")
            started = devicePanel.startChainRename ? devicePanel.startChainRename(target.rackId, target.chainId) : false
        else if (target.kind === "track")
            started = arrangement.renameTrack ? arrangement.renameTrack(target.trackId) : false
        else
            return  // (the session said why)
        if (!started)
            showMessage(qsTr("Select a track, a rack chain or a preset to rename."))
    }

    // Ctrl+F (from a plug-in's editor too): the browser shows, searching everything.
    function findInBrowser() {
        window.requestActivate()
        browserAction.checked = true
        Session.browser.focusSearch()
    }

    // Shift+Tab: the clip view goes back to the devices; or the selected clips open in it.
    function toggleClipView() {
        if (deviceArea.clipViewShown)
            deviceArea.showDevices()
        else if (arrangement.openClipView)
            arrangement.openClipView()
    }

    width: 1440
    height: 860
    visible: true
    title: Session.title

    Component.onCompleted: {
        windowState.restore()
        const splitterState = windowState.splitterState()
        if (splitterState)
            splitter.restoreState(splitterState)
        Qt.callLater(window.focusLanes)  // (the lanes take the keyboard once the window shows)
    }

    // Closing: a render running is cancelled instead (the window stays, as its
    // dialog's Cancel); else unsaved changes are asked about. The window's
    // place and the splitter are kept for next time.
    onClosing: close => {
        if (!closeConfirmed) {
            if (!Session.requestClose()) {
                close.accepted = false
                return
            }
            if (!Session.clean) {
                close.accepted = false
                confirmDiscard(() => {
                    window.closeConfirmed = true
                    window.close()
                })
                return
            }
        }
        windowState.save(splitter.saveState())
    }

    WindowState {
        id: windowState
        window: window
    }

    // The window's shortcuts while a plug-in's (Win32) editor has the focus (Windows only).
    PluginEditorKeys {
        id: pluginKeys
        target: window
        session: Session
    }

    Connections {
        target: Session

        function onStatusMessage(message) {
            window.showMessage(message)
        }
        function onWarning(message) {
            messages.warning(message)
        }
        function onInformation(message) {
            messages.information(message)
        }
        function onSaveAsRequested() {
            window.saveAs(window.afterSave)
            window.afterSave = null
        }
        function onProjectOpened() {
            if (arrangement.zoomToArrangement)
                arrangement.zoomToArrangement()
        }
    }

    Connections {
        target: Session.arrangement

        function onClipViewRequested(refs, leadTrackId, leadClipId) {
            deviceArea.openClips(refs, leadTrackId, leadClipId)
        }
    }

    // What the views have to say, for the status line.
    Connections {
        target: arrangement
        ignoreUnknownSignals: true

        function onStatusMessage(message) {
            window.showMessage(message)
        }
    }
    Connections {
        target: devicePanel
        ignoreUnknownSignals: true

        function onStatusMessage(message) {
            window.showMessage(message)
        }
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action { objectName: "newProject"; text: qsTr("&New Project"); shortcut: StandardKey.New; onTriggered: window.newProject() }
            Action { objectName: "openProject"; text: qsTr("&Open…"); shortcut: StandardKey.Open; onTriggered: window.openProject() }
            Menu {
                id: recentMenu
                objectName: "recentMenu"
                title: qsTr("Open &Recent")
                onAboutToShow: window.fillRecentMenu()
                MenuItem { text: qsTr("No Recent Projects"); enabled: false }
            }
            MenuSeparator {}
            Action { objectName: "save"; text: qsTr("&Save"); shortcut: StandardKey.Save; onTriggered: window.save() }
            Action { objectName: "saveAs"; text: qsTr("Save &As…"); shortcut: "Ctrl+Shift+S"; onTriggered: window.saveAs() }
            MenuSeparator {}
            Action { objectName: "exportAudio"; text: qsTr("&Export Audio…"); shortcut: "Ctrl+Shift+R"; onTriggered: exportDialog.open() }
            MenuSeparator {}
            Action { objectName: "quit"; text: qsTr("&Quit"); shortcut: "Ctrl+Q"; onTriggered: window.close() }
        }
        Menu {
            title: qsTr("&Edit")
            Action {
                id: undoAction
                objectName: "undo"
                text: Session.undoStack.undoText !== "" ? qsTr("&Undo %1").arg(Session.undoStack.undoText) : qsTr("&Undo")
                enabled: Session.undoStack.canUndo
                shortcut: StandardKey.Undo
                onTriggered: Session.undoStack.undo()
            }
            Action {
                id: redoAction
                objectName: "redo"
                text: Session.undoStack.redoText !== "" ? qsTr("&Redo %1").arg(Session.undoStack.redoText) : qsTr("&Redo")
                enabled: Session.undoStack.canRedo
                shortcut: "Ctrl+Y"
                onTriggered: Session.undoStack.redo()
            }
            MenuSeparator {}
            Action { objectName: "cut"; text: qsTr("Cu&t"); shortcut: StandardKey.Cut; onTriggered: Session.cut() }
            Action { objectName: "copy"; text: qsTr("&Copy"); shortcut: StandardKey.Copy; onTriggered: Session.copy() }
            Action { objectName: "paste"; text: qsTr("&Paste"); shortcut: StandardKey.Paste; onTriggered: Session.paste() }
            Action { objectName: "duplicate"; text: qsTr("D&uplicate"); shortcut: "Ctrl+D"; onTriggered: Session.duplicate() }
            Action { objectName: "rename"; text: qsTr("&Rename"); shortcut: "Ctrl+R"; onTriggered: window.rename() }
            Action { objectName: "split"; text: qsTr("&Split"); shortcut: "Ctrl+E"; onTriggered: Session.split() }
            Action { objectName: "consolidate"; text: qsTr("C&onsolidate"); shortcut: "Ctrl+J"; onTriggered: Session.consolidate() }
            Action { objectName: "reverseClips"; text: qsTr("Re&verse Clips"); shortcut: "R"; onTriggered: Session.reverseClips() }
            MenuSeparator {}
            Action { objectName: "freeze"; text: qsTr("&Freeze / Unfreeze Track"); shortcut: "Ctrl+Shift+F"; onTriggered: Session.toggleFreeze() }
            Action { objectName: "flatten"; text: qsTr("Flatten Track"); onTriggered: Session.flattenSelectedTracks() }
            Action { id: deleteAction; objectName: "delete"; text: qsTr("&Delete"); shortcut: StandardKey.Delete; onTriggered: Session.deleteSelection() }
            Action { objectName: "selectAll"; text: qsTr("Select &All"); shortcut: StandardKey.SelectAll; onTriggered: Session.selectAll() }
            MenuSeparator {}
            // Enabled while automation is overridden somewhere.
            Action {
                objectName: "reEnableAutomation"
                text: qsTr("Re-Enable Automation")
                enabled: Session.automationOverridden
                onTriggered: Session.bridge.reEnableAutomation()
            }
            Action { objectName: "soloSelectedTracks"; text: qsTr("Solo Selected Tracks"); shortcut: "S"; onTriggered: Session.soloSelectedTracks() }
            MenuSeparator {}
            Action { objectName: "playStop"; text: qsTr("Play / Stop"); shortcut: "Space"; onTriggered: Session.togglePlay() }
            Action { objectName: "record"; text: qsTr("Record"); shortcut: "F9"; onTriggered: Session.toggleRecord() }
            Menu {
                id: quantizeMenu
                objectName: "recordQuantization"
                title: qsTr("Record &Quantization")
                // Where recorded MIDI notes start: as played, or on this grid
                // (record/quantize in the preferences; checked from it).
                ActionGroup {
                    id: quantizeGroup
                }
                Instantiator {
                    model: Session.recordQuantizeChoices
                    delegate: Action {
                        required property var modelData
                        required property int index

                        // The check follows the session's choice (also after a click changed it).
                        function rebind() {
                            checked = Qt.binding(() => Math.abs(modelData.value - Session.recordQuantize) < 1e-9)
                        }

                        objectName: "quantize_" + index
                        text: modelData.label
                        checkable: true
                        checked: Math.abs(modelData.value - Session.recordQuantize) < 1e-9
                        ActionGroup.group: quantizeGroup
                        onTriggered: {
                            Session.recordQuantize = modelData.value
                            const actions = quantizeGroup.actions
                            for (let i = 0; i < actions.length; ++i)
                                actions[i].rebind()
                        }
                    }
                    onObjectAdded: (index, object) => quantizeMenu.insertAction(index, object)
                    onObjectRemoved: (index, object) => quantizeMenu.removeAction(object)
                }
            }
            Action { objectName: "goToStart"; text: qsTr("Go to Start"); shortcut: "Home"; onTriggered: Session.locate(0) }
            Action {
                id: loopAction
                objectName: "loop"
                text: qsTr("Loop")
                shortcut: "Ctrl+L"
                checkable: true
                checked: Session.project.loopEnabled
                onTriggered: {
                    Session.editor.setLoopEnabled(checked)
                    checked = Qt.binding(() => Session.project.loopEnabled)
                }
            }
            Action { objectName: "findInBrowser"; text: qsTr("Find in Browser"); shortcut: "Ctrl+F"; onTriggered: window.findInBrowser() }
        }
        Menu {
            title: qsTr("&Create")
            Action { objectName: "insertAudioTrack"; text: qsTr("Insert Audio &Track"); shortcut: "Ctrl+T"; onTriggered: Session.insertAudioTrack() }
            Action { objectName: "insertMidiTrack"; text: qsTr("Insert &MIDI Track"); shortcut: "Ctrl+Shift+T"; onTriggered: Session.insertMidiTrack() }
            Action { objectName: "insertReturnTrack"; text: qsTr("Insert &Return Track"); shortcut: "Ctrl+Alt+T"; onTriggered: Session.insertReturnTrack() }
            Action {
                id: insertMidiClipAction
                objectName: "insertMidiClip"
                text: qsTr("Insert MIDI &Clip")
                shortcut: "Ctrl+Shift+D"
                onTriggered: Session.insertMidiClip(window.insertGridStep())
            }
            MenuSeparator {}
            // (In the device view: its selected devices.)
            Action { objectName: "groupTracks"; text: qsTr("&Group Tracks"); shortcut: "Ctrl+G"; onTriggered: Session.groupSelected() }
            Action { objectName: "ungroupTracks"; text: qsTr("&Ungroup Tracks"); shortcut: "Ctrl+Shift+G"; onTriggered: Session.ungroupSelected() }
            MenuSeparator {}
            Action { objectName: "deleteSelectedTracks"; text: qsTr("Delete Selected Tracks"); onTriggered: Session.deleteSelectedTracks() }
        }
        Menu {
            title: qsTr("&View")
            Action { id: browserAction; objectName: "browser"; text: qsTr("&Browser"); shortcut: "Ctrl+Alt+B"; checkable: true; checked: true }
            Action { id: deviceViewAction; objectName: "deviceView"; text: qsTr("&Device View"); shortcut: "Ctrl+Alt+L"; checkable: true; checked: true }
            Action { objectName: "clipView"; text: qsTr("&Clip View"); shortcut: "Shift+Tab"; onTriggered: window.toggleClipView() }
            Action { objectName: "automation"; text: qsTr("&Automation"); shortcut: "A"; onTriggered: Session.editor.toggleAllAutomation() }
            // Plug-in editors are Win32 windows: closing the foremost is for Windows only.
            MenuItem {
                visible: pluginKeys.supported
                height: visible ? implicitHeight : 0
                action: Action {
                    objectName: "closePluginEditor"
                    text: qsTr("Close Plug-in &Editor")
                    shortcut: "Ctrl+W"
                    enabled: pluginKeys.supported
                    onTriggered: pluginKeys.closeForemostEditor()
                }
            }
            MenuSeparator {}
            Action { id: zoomInAction; objectName: "zoomIn"; text: qsTr("Zoom &In"); shortcut: "+"; onTriggered: window.zoom(1.4) }
            Action { id: zoomOutAction; objectName: "zoomOut"; text: qsTr("Zoom &Out"); shortcut: "-"; onTriggered: window.zoom(1 / 1.4) }
            Action {
                objectName: "zoomToArrangement"
                text: qsTr("Zoom to &Arrangement")
                shortcut: "Z"
                onTriggered: if (arrangement.zoomToArrangement) arrangement.zoomToArrangement()
            }
            MenuSeparator {}
            Action { objectName: "narrowGrid"; text: qsTr("Narrow Grid"); shortcut: "Ctrl+1"; onTriggered: if (arrangement.narrowGrid) arrangement.narrowGrid() }
            Action { objectName: "widenGrid"; text: qsTr("Widen Grid"); shortcut: "Ctrl+2"; onTriggered: if (arrangement.widenGrid) arrangement.widenGrid() }
            Action {
                id: snapAction
                objectName: "snapToGrid"
                text: qsTr("Snap to Grid")
                shortcut: "Ctrl+4"
                checkable: true
                checked: arrangement.snap !== false
                onTriggered: {
                    if (arrangement.snap !== undefined)
                        arrangement.snap = checked
                    checked = Qt.binding(() => arrangement.snap !== false)
                }
            }
        }
        Menu {
            title: qsTr("&Options")
            Action { objectName: "preferences"; text: qsTr("&Preferences…"); shortcut: "Ctrl+,"; onTriggered: preferences.open() }
            Action { objectName: "rescanPlugins"; text: qsTr("&Rescan Plug-ins"); onTriggered: Session.browser.rescanPlugins() }
            Action {
                objectName: "computerMidiKeyboard"
                text: qsTr("Computer &MIDI Keyboard")
                shortcut: "M"
                checkable: true
                checked: Session.computerKeyboard.enabled
                onTriggered: {
                    Session.computerKeyboard.enabled = checked
                    checked = Qt.binding(() => Session.computerKeyboard.enabled)
                }
            }
            MenuSeparator {}
            Action {
                objectName: "lockEnvelopes"
                text: qsTr("&Lock Envelopes")
                checkable: true
                checked: Session.project.automationLocked
                onTriggered: {
                    Session.editor.setAutomationLocked(checked)
                    checked = Qt.binding(() => Session.project.automationLocked)
                }
            }
        }
        Menu {
            title: qsTr("&Help")
            Action { objectName: "about"; text: qsTr("&About SUBstation"); onTriggered: aboutDialog.open() }
        }
    }

    // The actions' other shortcuts (an Action has one).
    Shortcut { sequences: ["Ctrl+Shift+Z"]; enabled: redoAction.enabled; onActivated: redoAction.trigger() }
    Shortcut { sequences: ["Backspace"]; onActivated: deleteAction.trigger() }
    Shortcut { sequences: ["Ctrl+Shift+M"]; onActivated: insertMidiClipAction.trigger() }
    Shortcut { sequences: ["=", StandardKey.ZoomIn]; onActivated: zoomInAction.trigger() }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: zoomOutAction.trigger() }

    header: TransportBar {
        id: transportBar
        arrangement: window.arrangementView
        onPreferencesRequested: preferences.open()
    }

    SplitView {
        id: splitter
        objectName: "splitter"
        anchors.fill: parent
        orientation: Qt.Horizontal

        BrowserPanel {
            id: browser
            visible: browserAction.checked
            SplitView.preferredWidth: 300
            SplitView.minimumWidth: 120
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

            // The device view, or the clip view while clips are open in it.
            Item {
                id: deviceArea
                objectName: "deviceArea"

                property bool clipViewShown: false

                // Opens clips in the clip view (the arrangement's clipViewRequested).
                function openClips(refs, leadTrackId, leadClipId) {
                    deviceViewAction.checked = true
                    clipView.trackId = leadTrackId
                    clipView.leadClipId = leadClipId
                    clipView.clipIds = refs
                    clipViewShown = true
                }

                // Back to the devices (the clip view's Esc or ×, Shift+Tab); the lanes take the keyboard.
                function showDevices() {
                    if (!clipViewShown)
                        return
                    clipViewShown = false
                    window.focusLanes()
                }

                visible: deviceViewAction.checked
                SplitView.preferredHeight: 280

                Placeholder {
                    id: devicePanel
                    objectName: "devicePanel"
                    anchors.fill: parent
                    visible: !deviceArea.clipViewShown
                    label: qsTr("Device View")
                    detail: qsTr("the selected track's devices")
                }

                ClipView {
                    id: clipView
                    objectName: "clipView"
                    anchors.fill: parent
                    visible: deviceArea.clipViewShown
                    onCloseRequested: deviceArea.showDevices()
                    onLocateRequested: beat => Session.locate(beat)
                    onStatusMessage: message => window.showMessage(message)
                }
            }
        }
    }

    // QStatusBar: PANEL with a BORDER line above, dim text; a project's
    // plug-ins loading at its right ("Loading plug-ins: 3 of 12", with a bar).
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
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Ready")
            color: Theme.textDim
            font: Theme.font
            elide: Text.ElideRight
        }

        Timer {
            id: messageTimer
            objectName: "messageTimer"
            interval: Session.statusTimeout
            onTriggered: statusText.text = ""
        }

        RowLayout {
            id: pluginsLoading
            objectName: "pluginsLoading"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: Session.pluginsTotal > 0
            spacing: 6

            Text {
                objectName: "pluginsLabel"
                text: Session.pluginsLoadingText
                color: Theme.textDim
                font: Theme.font
            }
            ProgressBar {
                objectName: "pluginsBar"
                Layout.preferredWidth: 120
                Layout.preferredHeight: 10
                from: 0
                to: Math.max(1, Session.pluginsTotal)
                value: Session.pluginsLoaded
            }
        }
    }

    // --- Dialogs -------------------------------------------------------------------------------

    UnsavedChangesDialog {
        id: unsavedDialog
        property var then: null
        onAnswered: value => {
            const then = unsavedDialog.then
            unsavedDialog.then = null
            if (value === "save")
                window.save(then)
            else if (value === "discard")
                then()
        }
    }

    // Session.warning and Session.information, one after another.
    MessageBox {
        id: messages
        objectName: "messageBox"

        property var queue: []

        function warning(text) {
            queue.push({text: text, icon: "warning"})
            showNext()
        }
        function information(text) {
            queue.push({text: text, icon: "information"})
            showNext()
        }
        function showNext() {
            if (visible || queue.length === 0)
                return
            const next = queue.shift()
            icon = next.icon
            show(next.text)
        }

        onClosed: Qt.callLater(showNext)
    }

    AboutDialog {
        id: aboutDialog
    }

    PreferencesDialog {
        id: preferences
    }

    ExportDialog {
        id: exportDialog
        onExportChosen: (range, bitDepth) => window.exportChosen(range, bitDepth)
    }

    RenderDialog {
        id: renderDialog
    }

    FileDialog {
        id: openDialog
        objectName: "openDialog"
        title: qsTr("Open Project")
        fileMode: FileDialog.OpenFile
        nameFilters: [Session.projectFilter]
        onAccepted: window.openChosen(FileUrls.localPath(selectedFile))
    }

    FileDialog {
        id: saveDialog
        objectName: "saveDialog"
        property var then: null
        title: qsTr("Save Project As")
        fileMode: FileDialog.SaveFile
        nameFilters: [Session.projectFilter]
        defaultSuffix: Session.projectExtension.replace(/^\./, "")
        onAccepted: window.saveChosen(FileUrls.localPath(selectedFile))
        onRejected: then = null
    }

    FileDialog {
        id: exportFileDialog
        objectName: "exportFileDialog"
        property string range: "arrangement"
        property int bitDepth: 24
        title: qsTr("Export Audio")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("WAV Audio (*.wav)")]
        defaultSuffix: "wav"
        onAccepted: window.exportFileChosen(FileUrls.localPath(selectedFile))
    }

    Component {
        id: recentEntry

        MenuItem {
            property string path
            property string tip
            property bool clearList: false
            onTriggered: {
                if (clearList)
                    Session.clearRecentProjects()
                else if (path !== "")
                    window.openRecent(path)
            }
            ToolTip.visible: hovered && tip !== ""
            ToolTip.text: tip
            ToolTip.delay: 700
        }
    }

    Component {
        id: menuSeparator

        MenuSeparator {}
    }
}
