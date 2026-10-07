import QtQuick
import QtQuick.Controls
import SUBstation

// The body of a rack: a strip of buttons (show the chain list, show the chain's
// devices beside the rack, add a macro, take the last away), its macros (4 by
// default, up to 16: two rows of them), and, while shown, its chains (a row
// each, with its mixer; a hint while it has none; + Chain adds one). The chain
// list is hidden until asked for; the chain clicked in it shows its devices
// beside the rack (the device view, after the frame), unless they are hidden.
// The list scrolls when there are more chains than room.
Item {
    id: body

    required property string trackId
    required property string deviceId
    required property var panel
    required property DeviceInfo info
    readonly property RackChains chains: rackChains
    readonly property RackMacros macroCount: rackMacros
    readonly property alias macros: macroRepeater
    readonly property alias rows: rowRepeater
    readonly property alias addButton: addButton
    readonly property alias chainList: list
    readonly property alias chainListButton: chainListButton
    readonly property alias devicesButton: devicesButton
    readonly property alias addMacroButton: addMacroButton
    readonly property alias removeMacroButton: removeMacroButton
    readonly property bool chainListShown: info.chainListShown

    // The strip, the macros (centred in at least as much width as a title needs), and the chain list while shown.
    readonly property real macrosLeft: strip.x + strip.width + 6
    readonly property real macrosRight: Math.max(macrosLeft + macroGrid.width, 190)
    implicitWidth: (chainListShown ? macrosRight + 12 + 214 : macrosRight) + 8
    implicitHeight: 6 + Math.max(macroGrid.implicitHeight, strip.implicitHeight, 60) + 6

    RackChains {
        id: rackChains
        session: Session
        trackId: body.trackId
        rackId: body.deviceId
    }
    RackMacros {
        id: rackMacros
        session: Session
        trackId: body.trackId
        rackId: body.deviceId
    }

    Column {
        id: strip
        x: 6
        y: 6
        spacing: 2

        DeviceHeaderButton {
            id: chainListButton
            objectName: "chainListButton"
            iconName: "chain_list"
            checkable: false
            checked: body.chainListShown
            tooltip: body.chainListShown ? qsTr("Hide the chain list") : qsTr("Show the chain list")
            onClicked: Session.deviceSelection.toggleChainList(body.deviceId)
        }
        DeviceHeaderButton {
            id: devicesButton
            objectName: "devicesButton"
            iconName: "rack_devices"
            checkable: false
            checked: body.info.rackDevicesShown
            tooltip: body.info.rackDevicesShown ? qsTr("Hide the chain's devices") : qsTr("Show the chain's devices")
            onClicked: Session.deviceSelection.toggleRackDevices(body.deviceId)
        }
        DeviceHeaderButton {
            id: addMacroButton
            objectName: "addMacroButton"
            checkable: false
            text: "+"
            enabled: rackMacros.count < rackMacros.maximum
            tooltip: qsTr("Add a macro")
            onClicked: rackMacros.add()
        }
        DeviceHeaderButton {
            id: removeMacroButton
            objectName: "removeMacroButton"
            checkable: false
            text: "−"
            enabled: rackMacros.count > 1
            tooltip: qsTr("Take the last macro away (with its mappings and automation)")
            onClicked: rackMacros.remove()
        }
    }

    // Two rows of macros (one while there is one), left to right, in the middle of the height.
    Grid {
        id: macroGrid
        objectName: "macroGrid"
        x: body.macrosLeft + (body.macrosRight - body.macrosLeft - width) / 2
        y: Math.max(6, Math.round((body.height - height) / 2))
        columns: Math.max(1, Math.ceil(rackMacros.count / 2))
        columnSpacing: 4
        rowSpacing: 4

        Repeater {
            id: macroRepeater
            model: rackMacros.count
            RackMacroKnob {
                required property int index
                objectName: "macro" + index
                trackId: body.trackId
                rackId: body.deviceId
                macroIndex: index
                panel: body.panel
            }
        }
    }

    // The chain list: PANEL inside a BORDER line.
    Rectangle {
        id: list
        objectName: "chainList"
        visible: body.chainListShown
        x: body.macrosRight + 12
        y: 6
        width: body.width - x - 8
        height: body.height - y - 6
        color: Theme.panel
        border.color: Theme.border

        Flickable {
            id: rowsView
            x: 1
            y: 1
            width: parent.width - 2
            height: addButton.y - 1 - y
            clip: true
            interactive: false
            contentWidth: width
            contentHeight: rowsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                id: rowsBar
                policy: rowsView.contentHeight > rowsView.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
            // The wheel scrolls it as a scroll area does: three steps of 20 px a notch.
            WheelHandler {
                acceptedModifiers: Qt.NoModifier
                onWheel: event => rowsView.scrollTo(rowsView.contentY - event.angleDelta.y / 120 * 60)
            }

            function scrollTo(y) {
                contentY = Math.max(0, Math.min(Math.max(0, contentHeight - height), y))
            }
            // The chain shown (its row selected) in view.
            function showShown() {
                const index = rackChains.chainIds.indexOf(rackChains.shownChain)
                if (index < 0)
                    return
                const top = index * 23
                if (top < contentY)
                    scrollTo(top)
                else if (top + 22 > contentY + height)
                    scrollTo(top + 22 - height)
            }
            Connections {
                target: rackChains
                function onShownChainChanged() {
                    Qt.callLater(rowsView.showShown)
                }
            }
            // (Once the rows are laid out.)
            onContentHeightChanged: Qt.callLater(rowsView.showShown)
            Component.onCompleted: Qt.callLater(rowsView.showShown)

            Column {
                id: rowsColumn
                width: rowsView.width - (rowsBar.policy === ScrollBar.AlwaysOn ? rowsBar.width : 0)
                spacing: 1

                Repeater {
                    id: rowRepeater
                    model: body.chainListShown ? rackChains.chainIds : []
                    RackChainRow {
                        required property string modelData
                        objectName: "chainRow_" + modelData
                        width: rowsColumn.width
                        trackId: body.trackId
                        rackId: body.deviceId
                        chainId: modelData
                        selected: rackChains.shownChain === modelData
                        panel: body.panel
                    }
                }
                Text {
                    objectName: "chainsHint"
                    visible: rackChains.count === 0
                    width: rowsColumn.width
                    leftPadding: 2
                    text: qsTr("No chains: the rack passes its input on.\nDrop devices here, or add a chain.")
                    wrapMode: Text.Wrap
                    color: Theme.textDisabled
                    font: Theme.uiFont(8)
                }
            }
        }

        Button {
            id: addButton
            objectName: "addChain"
            x: 1
            width: parent.width - 2
            y: parent.height - 1 - height
            text: qsTr("+ Chain")
            focusPolicy: Qt.NoFocus
            onClicked: rackChains.addChain()

            HoverHandler {
                id: addHover
            }
            ToolTip.visible: addHover.hovered
            ToolTip.text: qsTr("Add a chain to the rack")
            ToolTip.delay: 700
        }
    }
}
