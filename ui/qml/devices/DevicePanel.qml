import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import SUBstation

// The device view (device_panel/panel.py's DevicePanel), below the
// arrangement: the selected track's (or a return's, or the master's) chain of
// devices, left to right, built-in devices and plug-ins alike, racks with
// their macros and chains, and after a rack the chain it shows, in a bracket
// (DeviceChain, DeviceFrame); what may be dropped there, or that no track is
// selected, or that it is frozen (Session.deviceSelection.hint). It scrolls
// sideways without a scroll bar: Shift+wheel, or Ctrl+Alt-drag anywhere on it
// (DeviceChainArea, which also takes the drops and starts the drags).
//
// What it is to the main window:
// - startChainRename(rackId, chainId): Ctrl+R on a rack's chain
//   (Session.renameTarget()): its row's name edited in place; false if the
//   chain isn't shown.
// - focusDevices(): the device view takes the focus (Delete, the clipboard
//   and Ctrl+G act on its devices).
// - statusMessage(text): what it has to say beyond what Session.statusMessage
//   says (the device selection's messages go there).
// - implicitHeight: the height it needs (the tallest device, a page of knobs
//   with page arrows, and room for editors' graphs); it never scrolls
//   vertically.
//
// Clicking beside the devices deselects them but keeps the focus here (Ctrl+V
// pastes into this track); right-click there to paste or load a preset file
// at that place. The device, sidechain, chain and macro menus and the dialogs
// (Save Preset, replacing one, the preset files) are the panel's own.
Rectangle {
    id: panel

    signal statusMessage(string message)

    readonly property alias area: area
    readonly property alias menu: menu
    readonly property alias chain: topChain
    readonly property alias hint: hint
    readonly property alias dropMarker: dropMarker
    readonly property alias loadMarker: loadMarker
    readonly property alias presetNameDialog: nameDialog
    readonly property alias presetReplaceDialog: replaceDialog
    readonly property int panelMargin: 8  // PANEL_MARGIN: above and below the chain
    readonly property int extraHeight: 12  // EXTRA_HEIGHT: what the devices get beyond the tallest's need
    // The tallest device: its border, a title bar, a page of knobs in two rows.
    readonly property int deviceHeight: 2 + Math.max(16, titleMetrics.height) + 4 + 6 + 2 * probe.implicitHeight + 6 + 4

    // Ctrl+R on a rack's chain: its name edited in place (false: it isn't shown).
    function startChainRename(rackId, chainId) {
        const row = area.chainRowOf(rackId, chainId)
        if (!row)
            return false
        area.scrollTo(rackId)
        row.startRename()
        return true
    }

    // The device view takes the focus: the Edit menu acts on its devices.
    function focusDevices() {
        if (Session.deviceSelection.trackId !== "")
            Session.selection.focusDevices()
        panel.forceActiveFocus()
    }

    // --- Menus ---------------------------------------------------------------------------------

    // A device's right-click menu (frame.py's contextMenuEvent): its own
    // entries (a plug-in's editor and VST3 presets, a rack's Add Chain, an
    // editor's), Fold, the clipboard's (for the selected devices), Move Left
    // and Right, presets, Group and Ungroup, Delete.
    function showDeviceMenu(frame, at, x, y) {
        const id = frame.deviceId
        const trackId = frame.trackId
        const info = frame.info
        const devices = Session.deviceSelection
        menu.reset()
        if (info.isPlugin) {
            menu.entry(qsTr("Show Editor"), () => Session.bridge.openPluginEditor(trackId, id), undefined, info.loaded)
            menu.entry(qsTr("Load VST3 Preset…"), () => panel.loadVst3Preset(id), undefined, info.loaded)
            menu.entry(qsTr("Save VST3 Preset…"), () => panel.saveVst3Preset(id, info.name), undefined, info.loaded)
            menu.separator()
        }
        if (info.isRack) {
            menu.entry(qsTr("Add Chain"), () => Session.editor.tryAddRackChain(trackId, id))
            menu.separator()
        }
        const body = frame.body
        if (body && body.menuActions !== undefined) {
            for (let i = 0; i < body.menuActions.length; ++i)
                menu.addAction(body.menuActions[i])
        }
        menu.entry(info.folded ? qsTr("Unfold") : qsTr("Fold"), () => devices.toggleFold(id))
        menu.separator()
        menu.hinted(qsTr("Cut"), () => devices.cutSelected(), "Ctrl+X")
        menu.hinted(qsTr("Copy"), () => devices.copySelected(), "Ctrl+C")
        menu.hinted(qsTr("Paste"), () => devices.pasteAfter(id), "Ctrl+V", devices.hasClipboard)
        menu.hinted(qsTr("Duplicate"), () => devices.duplicateSelected(), "Ctrl+D")
        menu.separator()
        if (!info.instrument) {  // (an instrument doesn't move, and nothing goes before it)
            const index = devices.chainDevices(info.chainId).indexOf(id)
            menu.entry(qsTr("Move Left"), () => Session.editor.moveDevice(trackId, id, index - 1), undefined,
                       info.canMoveLeft)
            menu.entry(qsTr("Move Right"), () => Session.editor.moveDevice(trackId, id, index + 1), undefined,
                       info.canMoveRight)
            menu.separator()
        }
        menu.entry(qsTr("Save Preset…"), () => panel.savePreset(id))
        if (!info.isRack) {
            menu.entry(qsTr("Save as Default Preset"), () => devices.saveAsDefault(id))
            menu.entry(qsTr("Clear Default Preset"), () => devices.clearDefault(id), undefined, devices.hasDefault(id))
        }
        menu.separator()
        menu.hinted(qsTr("Group"), () => devices.groupDevice(id), "Ctrl+G")
        if (info.isRack)
            menu.hinted(qsTr("Ungroup"), () => devices.ungroupRack(id), "Ctrl+Shift+G")
        menu.separator()
        menu.entry(qsTr("Delete"), () => devices.deleteSelected())
        menu.popup(at, x, y)
    }

    // A device's sidechain menu: No Sidechain, the tracks it can come from
    // (those that would close a cycle greyed out), then where it is taken.
    function showSidechainMenu(frame, at, x, y) {
        const trackId = frame.trackId
        const id = frame.deviceId
        menu.reset()
        const entries = frame.info.sidechainMenu()
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (entry.separator) {
                menu.separator()
                continue
            }
            menu.entry(entry.text, () => Session.editor.trySetDeviceSidechain(trackId, id, entry.source, entry.tap),
                       entry.checked, entry.enabled)
        }
        menu.popup(at, x, y)
    }

    // A rack chain's menu; after it (whatever was chosen, but Rename, and
    // unless the chain went) the chain shows beside its rack.
    function showChainMenu(row, x, y) {
        const chain = row.chain
        const trackId = row.trackId
        const rackId = row.rackId
        const chainId = row.chainId
        let renaming = false
        menu.reset()
        menu.entry(qsTr("Rename"), () => renaming = true)
        menu.entry(qsTr("Duplicate"), () => Session.editor.duplicateRackChain(trackId, chainId))
        menu.entry(qsTr("Delete"), () => Session.editor.removeRackChains(trackId, [chainId]))
        menu.separator()
        menu.entry(qsTr("Add Chain"), () => Session.editor.tryAddRackChain(trackId, rackId))
        menu.separator()
        menu.entry(qsTr("Show Volume Automation"), () => chain.showVolumeAutomation())
        menu.entry(qsTr("Show Pan Automation"), () => chain.showPanAutomation())
        menu.afterClose = () => {
            if (renaming)
                row.startRename()
            else if (area.hasChain(rackId, chainId))
                Session.deviceSelection.clickChain(rackId, chainId)
        }
        menu.popup(row, x, y)
    }

    // A macro's menu: what it moves, to unmap.
    function showMacroMenu(macro, at, x, y) {
        const trackId = macro.trackId
        const rackId = macro.rackId
        menu.reset()
        const entries = macro.unmapEntries()
        if (entries.length === 0)
            menu.entry(qsTr("Nothing mapped (right-click a parameter in the rack to map it)"), () => {}, undefined, false)
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            menu.entry(entry.text, () => Session.editor.unmapMacro(trackId, rackId, entry.deviceId, entry.paramId))
        }
        menu.popup(at, x, y)
    }

    // Beside the devices (at x, y of the chain's area): paste there, or load a preset there.
    function showBesideMenu(x, y) {
        const devices = Session.deviceSelection
        if (devices.trackId === "")
            return
        const target = area.dropTarget(x, y)
        Session.selection.focusDevices()
        menu.reset()
        menu.entry(qsTr("Paste"), () => devices.pasteAt(target.chain, target.index), undefined, devices.hasClipboard)
        menu.separator()
        menu.entry(qsTr("Load Preset…"), () => panel.loadPresetFile(target.chain, target.index))
        menu.popup(area, x, y)
    }

    // --- Presets -------------------------------------------------------------------------------

    // The save button (and Save Preset…): the device saved to the library under
    // a name asked for (its name to start with), asking before replacing one.
    function savePreset(deviceId) {
        nameDialog.deviceId = deviceId
        nameField.text = Session.deviceSelection.presetName(deviceId)
        nameDialog.open()
    }

    // The name given: saved, unless it can't be a preset's (said why) or one
    // of that name is there (asked first).
    function savePresetNamed(deviceId, name) {
        const check = Session.deviceSelection.checkPresetName(deviceId, name)
        if (check.error !== undefined) {
            panel.statusMessage(check.error)
            return
        }
        if (check.exists) {
            replaceDialog.deviceId = deviceId
            replaceDialog.name = name
            replaceDialog.question = check.question
            replaceDialog.open()
            return
        }
        Session.deviceSelection.savePreset(deviceId, name)
    }

    // Load Preset… beside the devices: a preset file, as a new device at that place.
    function loadPresetFile(chain, index) {
        presetFileDialog.chain = chain
        presetFileDialog.index = index
        presetFileDialog.currentFolder = area.fileUrl(Session.deviceSelection.presetFolder())
        presetFileDialog.open()
    }

    function loadVst3Preset(deviceId) {
        vst3LoadDialog.deviceId = deviceId
        vst3LoadDialog.currentFolder = area.fileUrl(Session.deviceSelection.vst3PresetFolder(deviceId))
        vst3LoadDialog.open()
    }

    function saveVst3Preset(deviceId, name) {
        const folder = Session.deviceSelection.vst3PresetFolder(deviceId)
        vst3SaveDialog.deviceId = deviceId
        vst3SaveDialog.currentFolder = area.fileUrl(folder)
        vst3SaveDialog.selectedFile = area.fileUrl(folder + "/" + name + ".vstpreset")
        vst3SaveDialog.open()
    }

    implicitHeight: 2 * panelMargin + deviceHeight + extraHeight + Theme.scrollBarWidth
    color: Theme.panel

    // A BORDER line above it.
    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
        z: 1
    }

    // A click beside the devices.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.LeftButton) {
                Session.deviceSelection.clickBeside()
            } else {
                const at = mapToItem(area, mouse.x, mouse.y)
                panel.showBesideMenu(at.x, at.y)
            }
        }
    }

    DeviceChainArea {
        id: area
        objectName: "chainArea"
        x: 10
        y: panel.panelMargin
        width: panel.width - 20
        height: panel.height - 2 * panel.panelMargin
        session: Session
        content: chainContent
        clip: true

        Row {
            id: chainContent
            objectName: "chainContent"
            x: -area.contentX
            height: area.height
            spacing: 6

            DeviceChain {
                id: topChain
                objectName: "chain"
                height: chainContent.height
                visible: count > 0
                chainId: ""
                panel: panel
            }

            Text {
                id: hint
                objectName: "hint"
                height: chainContent.height
                visible: text !== ""
                text: Session.deviceSelection.hint
                verticalAlignment: Text.AlignVCenter
                color: Theme.textDisabled
                font: Theme.font
            }
        }

        // Where dragged devices would go: a line between devices; or the device a preset would load into, outlined.
        Item {
            x: -area.contentX
            Rectangle {
                id: dropMarker
                objectName: "dropMarker"
                visible: area.dropMarker.width > 0
                x: area.dropMarker.x
                y: area.dropMarker.y
                width: area.dropMarker.width
                height: area.dropMarker.height
                color: Theme.accent
            }
            Rectangle {
                id: loadMarker
                objectName: "loadMarker"
                visible: area.loadMarker.width > 0
                x: area.loadMarker.x
                y: area.loadMarker.y
                width: area.loadMarker.width
                height: area.loadMarker.height
                color: "transparent"
                radius: 4
                border.width: 2
                border.color: Theme.accent
            }
        }
    }

    // Measured, not shown: a parameter's cell, for the tallest device's height.
    DeviceParamKnob {
        id: probe
        visible: false
    }
    FontMetrics {
        id: titleMetrics
        font: Theme.uiFont(9, true)
    }

    PanelMenu {
        id: menu
        objectName: "panelMenu"
    }

    Dialog {
        id: nameDialog
        objectName: "presetNameDialog"

        property string deviceId

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Save Preset")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            nameField.forceActiveFocus()
            nameField.selectAll()
        }
        onAccepted: panel.savePresetNamed(deviceId, nameField.text)

        Column {
            spacing: 6

            Label {
                text: qsTr("Preset name:")
            }
            TextField {
                id: nameField
                objectName: "presetNameField"
                width: 260
                onAccepted: nameDialog.accept()
            }
        }
    }

    Dialog {
        id: replaceDialog
        objectName: "presetReplaceDialog"

        property string deviceId
        property string name
        property alias question: questionLabel.text

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Save Preset")
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: Session.deviceSelection.savePreset(deviceId, name)

        Column {
            Label {
                id: questionLabel
                width: Math.min(questionMetrics.advanceWidth + 2, 420)
                wrapMode: Text.Wrap

                TextMetrics {
                    id: questionMetrics
                    font: questionLabel.font
                    text: questionLabel.text
                }
            }
        }
    }

    FileDialog {
        id: presetFileDialog
        objectName: "presetFileDialog"

        property string chain
        property int index: -1

        title: qsTr("Load Preset")
        nameFilters: [qsTr("SUBstation Preset (*.gilpreset)")]
        onAccepted: Session.deviceSelection.loadPresetFile(area.localPath(selectedFile), chain, index)
    }

    FileDialog {
        id: vst3LoadDialog
        objectName: "vst3LoadDialog"

        property string deviceId

        title: qsTr("Load VST3 Preset")
        nameFilters: [qsTr("VST3 Preset (*.vstpreset)")]
        onAccepted: Session.deviceSelection.loadVst3Preset(deviceId, area.localPath(selectedFile))
    }

    FileDialog {
        id: vst3SaveDialog
        objectName: "vst3SaveDialog"

        property string deviceId

        title: qsTr("Save VST3 Preset")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("VST3 Preset (*.vstpreset)")]
        onAccepted: Session.deviceSelection.saveVst3Preset(deviceId, area.localPath(selectedFile))
    }
}
