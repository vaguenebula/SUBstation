import QtQuick
import QtQuick.Controls
import SUBstation

// The body of a rack: its eight macros, four to a row, and its chains (a row
// each, with its mixer; a hint while it has none; + Chain adds one). The chain
// clicked shows its devices beside the rack (the device view, after the frame);
// the list scrolls when there are more chains than room.
Item {
    id: body

    required property string trackId
    required property string deviceId
    required property var panel
    readonly property RackChains chains: rackChains
    readonly property alias macros: macroRepeater
    readonly property alias rows: rowRepeater
    readonly property alias addButton: addButton

    implicitWidth: 420 - 2  // RACK_WIDTH
    implicitHeight: 6 + Math.max(macroGrid.implicitHeight, 60) + 6

    RackChains {
        id: rackChains
        session: Session
        trackId: body.trackId
        rackId: body.deviceId
    }

    Grid {
        id: macroGrid
        x: 8
        y: 6
        columns: 4
        columnSpacing: 4
        rowSpacing: 2

        Repeater {
            id: macroRepeater
            model: 8  // MACRO_COUNT
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
        x: macroGrid.x + macroGrid.width + 12
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
                    model: rackChains.chainIds
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
