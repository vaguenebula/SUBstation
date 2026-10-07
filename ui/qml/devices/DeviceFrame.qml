import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// A device in the device view, as Ableton draws one: its frame (PANEL_ALT in
// a line of its title bar's colour, 3 px corners), its title bar (DEVICE_HEADER,
// the selection's teal while selected: the fold triangle, the round on/off
// switch, the name with its tooltip, a plug-in's editor button,
// the sidechain button of a device with a sidechain input, the page arrows
// while there is more than one page, the save button) and its body: a page of
// knobs (a built-in device without an editor of its own), its editor
// (DeviceEditors), a plug-in's parameters (or why it shows none), or a rack's
// macros and (while shown) its chain list. Folded, it is a narrow strip: the
// triangle, the switch and the name reading upwards. Clicks on its background,
// title bar and labels select it, drag it, open its menu (DeviceFrameInput);
// the menus and dialogs are the panel's. A grip stands in the gap after it.
Item {
    id: frame

    readonly property string panelRole: "device"
    property string trackId
    property string deviceId
    property string chainId  // the chain it is in ("": the track's own)
    property var panel
    readonly property DeviceInfo info: deviceInfo
    readonly property bool selected: Session.deviceSelection.selected.indexOf(deviceId) >= 0
    // Its editor's file ("": a page of knobs, a plug-in or a rack).
    readonly property string editorUrl: deviceInfo.exists && !deviceInfo.isRack && !deviceInfo.isPlugin
                                        ? DeviceEditors.editorFor(deviceInfo.kind) : ""
    readonly property Item body: bodyLoader.item
    readonly property int pages: body && body.pages !== undefined ? body.pages : 1
    readonly property int page: body && body.page !== undefined ? body.page : 0
    // A rack's chain shown beside it (not while folded, nor while it hides its devices).
    readonly property bool showsChain: deviceInfo.isRack && !deviceInfo.folded && deviceInfo.rackDevicesShown
                                       && rackChains.count > 0 && rackChains.shownChain !== ""
    readonly property string shownChain: rackChains.shownChain
    readonly property real headerHeight: Math.max(16, titleMetrics.height) + 4
    // DEVICE_WIDTH; a rack's or an editor's own (a rack's with its macros, and its chain list while shown);
    // FOLDED_WIDTH folded.
    readonly property real unfoldedWidth: (deviceInfo.isRack || editorUrl !== "") && body ? body.implicitWidth + 2
                                         : deviceInfo.isRack ? 200 : 216
    readonly property alias input: input
    readonly property alias foldButton: foldButton
    readonly property alias enableButton: enableButton
    readonly property alias title: title
    readonly property alias editButton: editButton
    readonly property alias sidechainButton: sidechainButton
    readonly property alias previousButton: previousButton
    readonly property alias pageLabel: pageLabel
    readonly property alias nextButton: nextButton
    readonly property alias saveButton: saveButton
    readonly property alias foldedBar: foldedBar
    readonly property alias header: header
    readonly property color headerColor: selected ? Theme.deviceHeaderSelected : Theme.deviceHeader

    width: deviceInfo.folded ? 26 : unfoldedWidth

    // Another page of its parameters (remembered for the device across rebuilds).
    function setPage(page) {
        if (!body || body.page === undefined || !panel)
            return
        page = Math.max(0, Math.min(page, pages - 1))
        if (page !== body.page) {
            body.page = page
            panel.area.setPage(deviceId, page)
        }
    }

    function restorePage() {
        if (body && body.page !== undefined && body.pages !== undefined && panel)
            body.page = Math.max(0, Math.min(panel.area.pageOf(deviceId), body.pages - 1))
    }

    function loadBody() {
        const props = { trackId: frame.trackId, deviceId: frame.deviceId }
        if (deviceInfo.isRack) {
            props.panel = frame.panel
            props.info = deviceInfo
            bodyLoader.setSource(Qt.resolvedUrl("RackDeviceBody.qml"), props)
        } else if (deviceInfo.isPlugin) {
            props.info = deviceInfo
            bodyLoader.setSource(Qt.resolvedUrl("PluginDeviceBody.qml"), props)
        } else if (editorUrl !== "") {
            bodyLoader.setSource(editorUrl, props)
        } else {
            bodyLoader.setSource(Qt.resolvedUrl("DeviceKnobPages.qml"), props)
        }
    }

    Component.onCompleted: loadBody()

    DeviceInfo {
        id: deviceInfo
        session: Session
        trackId: frame.trackId
        deviceId: frame.deviceId
    }
    RackChains {
        id: rackChains
        session: deviceInfo.isRack ? Session : null
        trackId: frame.trackId
        rackId: frame.deviceId
    }
    FontMetrics {
        id: titleMetrics
        font: Theme.uiFont(9, true)
    }

    // The frame: PANEL_ALT in a 1 px line of the title bar's colour, 3 px corners.
    Rectangle {
        anchors.fill: parent
        color: Theme.panelAlt
        radius: 3
        border.width: 1
        border.color: frame.headerColor
    }

    // The grip in the gap after it (DeviceChainArea.kSpacing wide).
    DeviceGrip {
        x: frame.width + 2
        height: frame.height
    }

    // The on/off switch: a round one.
    component Activator: ToggleButton {
        id: activator
        role: "activator"
        checkable: false
        tooltip: qsTr("Device On/Off")
        background: Rectangle {
            radius: width / 2
            color: activator.look.background
            border.width: 1
            border.color: Theme.border
        }
    }

    // Clicks that reach the frame are on its background or labels: the controls take their own.
    DeviceFrameInput {
        id: input
        anchors.fill: parent
        session: Session
        trackId: frame.trackId
        deviceId: frame.deviceId
        chainArea: frame.panel ? frame.panel.area : null
        frame: frame
        onMenuRequested: (x, y) => frame.panel.showDeviceMenu(frame, input, x, y)
    }

    // The title bar.
    Item {
        id: header
        objectName: "header"
        visible: !deviceInfo.folded
        x: 1
        y: 1
        width: frame.width - 2
        height: frame.headerHeight

        // DEVICE_HEADER (teal while selected), its top corners rounded.
        Rectangle {
            anchors.fill: parent
            radius: 2
            color: frame.headerColor
        }
        Rectangle {
            y: parent.height / 2
            width: parent.width
            height: parent.height - y
            color: frame.headerColor
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 3
            anchors.rightMargin: 3
            spacing: 3

            DeviceHeaderButton {
                id: foldButton
                objectName: "foldButton"
                iconName: "fold"
                checkable: false
                checked: deviceInfo.folded  // (its picture: pointing right while folded)
                lit: false
                tooltip: deviceInfo.folded ? qsTr("Unfold") : qsTr("Fold")
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: Session.deviceSelection.toggleFold(frame.deviceId)
            }
            Activator {
                id: enableButton
                objectName: "enableButton"
                checked: deviceInfo.enabled
                Layout.preferredWidth: 13
                Layout.preferredHeight: 13
                onClicked: deviceInfo.setEnabled(!deviceInfo.enabled)
            }
            Text {
                id: title
                objectName: "title"
                Layout.fillWidth: true
                Layout.minimumWidth: 16
                Layout.leftMargin: 2
                text: deviceInfo.name
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                color: Theme.text
                font: Theme.uiFont(9, true)

                HoverHandler {
                    id: titleHover
                }
                ToolTip.visible: titleHover.hovered && deviceInfo.toolTip !== ""
                ToolTip.text: deviceInfo.toolTip
                ToolTip.delay: 700
            }
            DeviceHeaderButton {
                id: editButton
                objectName: "editButton"
                visible: deviceInfo.isPlugin
                iconName: "plugin_window"
                checkable: false
                checked: deviceInfo.editorOpen
                enabled: deviceInfo.loaded
                tooltip: qsTr("Show the plug-in's own editor")
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: deviceInfo.showEditor(!deviceInfo.editorOpen)
            }
            DeviceHeaderButton {
                id: sidechainButton
                objectName: "sidechainButton"
                visible: deviceInfo.hasSidechainInput
                iconName: "sidechain"
                checkable: false
                checked: deviceInfo.sidechainOn
                tooltip: deviceInfo.sidechainToolTip
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: frame.panel.showSidechainMenu(frame, sidechainButton, 0, sidechainButton.height)
            }
            DeviceHeaderButton {
                id: previousButton
                objectName: "previousPage"
                visible: frame.pages > 1
                enabled: frame.page > 0
                text: "‹"
                tooltip: qsTr("Previous parameters")
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: frame.setPage(frame.page - 1)
            }
            Text {
                id: pageLabel
                objectName: "pageLabel"
                visible: frame.pages > 1
                text: (frame.page + 1) + "/" + frame.pages
                color: Theme.textDim
                font: Theme.uiFont(8)
            }
            DeviceHeaderButton {
                id: nextButton
                objectName: "nextPage"
                visible: frame.pages > 1
                enabled: frame.page < frame.pages - 1
                text: "›"
                tooltip: qsTr("Next parameters")
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: frame.setPage(frame.page + 1)
            }
            DeviceHeaderButton {
                id: saveButton
                objectName: "saveButton"
                iconName: "save"
                tooltip: qsTr("Save Preset")
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                onClicked: frame.panel.savePreset(frame.deviceId)
            }
        }
    }

    // Folded: a strip instead, the fold button, the switch and the name reading upwards.
    Item {
        id: foldedBar
        objectName: "foldedBar"
        visible: deviceInfo.folded
        x: 1
        y: 1
        width: frame.width - 2
        height: frame.height - 2

        Rectangle {
            anchors.fill: parent
            radius: 2
            color: frame.headerColor
        }

        DeviceHeaderButton {
            objectName: "foldedFoldButton"
            x: (parent.width - width) / 2
            y: 4
            iconName: "fold"
            checkable: false
            checked: deviceInfo.folded
            lit: false
            tooltip: qsTr("Unfold")
            onClicked: Session.deviceSelection.toggleFold(frame.deviceId)
        }
        Activator {
            objectName: "foldedEnableButton"
            x: (parent.width - width) / 2
            y: 4 + 16 + 4
            width: 13
            height: 13
            checked: deviceInfo.enabled
            onClicked: deviceInfo.setEnabled(!deviceInfo.enabled)
        }
        // The name, reading upwards from near the top.
        Item {
            id: verticalTitle
            objectName: "verticalTitle"
            x: 2
            y: 4 + 16 + 4 + 13 + 4
            width: parent.width - 4
            height: Math.max(16, parent.height - y - 4)
            clip: true

            Text {
                anchors.centerIn: parent
                width: verticalTitle.height
                height: verticalTitle.width
                rotation: -90
                text: deviceInfo.name
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                color: Theme.text
                font: Theme.uiFont(9, true)
            }
        }
    }

    // The body (what folding hides): it takes all the height there is.
    Item {
        id: bodyArea
        objectName: "body"
        visible: !deviceInfo.folded
        x: 1
        y: 1 + frame.headerHeight
        width: frame.width - 2
        height: frame.height - y - 1

        Loader {
            id: bodyLoader
            anchors.fill: parent
            onLoaded: frame.restorePage()
        }
    }

    Connections {
        target: bodyLoader.item
        ignoreUnknownSignals: true
        function onPagesChanged() {
            frame.restorePage()
        }
        // The Sidechain's hint over its curve.
        function onSidechainMenuRequested() {
            frame.panel.showSidechainMenu(frame, bodyLoader.item, bodyLoader.item.width / 2, bodyLoader.item.height / 2)
        }
    }
}
