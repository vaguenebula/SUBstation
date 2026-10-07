import QtQuick
import QtQuick.Controls
import SUBstation

// A macro's mappings, where the range each parameter moves over is set: Min
// and Max (0 to 100 % of the parameter's own range, its value in its units
// beside them; Max below Min turns it the other way round: 100 to 0 %), Invert
// (swaps them), and Unmap. Each drag of a box is one undo step. Opened over its
// macro by the macro's menu (Edit Mappings…), it closes on Escape or a click
// elsewhere.
Popup {
    id: popup

    objectName: "macroMappings"
    property RackMacro macro: null
    readonly property alias rows: rowRepeater

    // Where it opened: the item it is about (its top left, in the overlay, and its height).
    property point anchorTop: Qt.point(0, 0)
    property real anchorHeight: 0

    // Opens it for `macro`, above `at` (an item of the macro; the device view is
    // at the window's bottom), or below it if there is no room above.
    function show(macro, at) {
        popup.macro = macro
        anchorTop = at.mapToItem(Overlay.overlay, 0, 0)
        anchorHeight = at.height
        open()
    }

    parent: Overlay.overlay
    // (Bound, so they follow its size once it is laid out.)
    x: Math.max(4, Math.min(anchorTop.x, (parent ? parent.width : 0) - width - 4))
    y: anchorTop.y - height - 4 >= 4 ? anchorTop.y - height - 4
                                     : Math.min(anchorTop.y + anchorHeight + 4, (parent ? parent.height : 0) - height - 4)
    focus: true
    padding: 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onMacroChanged: if (!macro) close()  // (its rack went)

    background: Rectangle {
        color: Theme.panelAlt
        radius: 4
        border.color: Theme.border
    }

    contentItem: Column {
        spacing: 6

        Text {
            objectName: "title"
            text: popup.macro ? qsTr("%1 moves").arg(popup.macro.name) : ""
            color: Theme.text
            font: Theme.uiFont(9, true)
        }
        Text {
            visible: !popup.macro || popup.macro.mappingKeys.length === 0
            text: qsTr("Nothing mapped: right-click a parameter of a device in the rack to map it.")
            color: Theme.textDisabled
            font: Theme.font
        }

        Repeater {
            id: rowRepeater
            model: popup.macro ? popup.macro.mappingKeys : []

            Row {
                id: row

                required property int index
                required property string modelData
                // Its mapping as it is now (its range, its texts).
                readonly property var entry: popup.macro && index < popup.macro.mappingList.length
                                             ? popup.macro.mappingList[index] : ({})
                readonly property alias low: low
                readonly property alias high: high
                readonly property alias invertButton: invertButton
                readonly property alias unmapButton: unmapButton

                objectName: "mapping_" + modelData
                spacing: 6
                height: 22

                Text {
                    width: 150
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: row.entry.name || ""
                    color: Theme.text
                    font: Theme.font
                }
                Text {
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Min")
                    color: Theme.textDim
                    font: Theme.uiFont(8)
                }
                ValueBox {
                    id: low
                    objectName: "low"
                    width: 52
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    from: 0
                    to: 100
                    step: 1
                    decimals: 0
                    defaultValue: 0
                    sampleText: "100 %"
                    formatter: v => Math.round(v) + " %"
                    value: (row.entry.low || 0) * 100
                    onMoved: (v, key) => popup.macro.setRange(row.entry.deviceId, row.entry.paramId, v / 100,
                                                              row.entry.high, key)
                }
                Text {
                    width: 70
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: row.entry.lowText || ""
                    color: Theme.textDim
                    font: Theme.uiFont(8)
                }
                Text {
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Max")
                    color: Theme.textDim
                    font: Theme.uiFont(8)
                }
                ValueBox {
                    id: high
                    objectName: "high"
                    width: 52
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    from: 0
                    to: 100
                    step: 1
                    decimals: 0
                    defaultValue: 100
                    sampleText: "100 %"
                    formatter: v => Math.round(v) + " %"
                    value: (row.entry.high !== undefined ? row.entry.high : 1) * 100
                    onMoved: (v, key) => popup.macro.setRange(row.entry.deviceId, row.entry.paramId, row.entry.low,
                                                              v / 100, key)
                }
                Text {
                    width: 70
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: row.entry.highText || ""
                    color: Theme.textDim
                    font: Theme.uiFont(8)
                }
                Button {
                    id: invertButton
                    objectName: "invert"
                    height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Invert")
                    focusPolicy: Qt.NoFocus
                    onClicked: popup.macro.setRange(row.entry.deviceId, row.entry.paramId, row.entry.high,
                                                    row.entry.low)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Swap Min and Max: the parameter goes the other way round")
                    ToolTip.delay: 700
                }
                Button {
                    id: unmapButton
                    objectName: "unmap"
                    height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Unmap")
                    focusPolicy: Qt.NoFocus
                    onClicked: popup.macro.unmap(row.entry.deviceId, row.entry.paramId)
                }
            }
        }
    }
}
