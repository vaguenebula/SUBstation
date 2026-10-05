import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import SUBstation

// The selected band's controls (eq.py's BandPanel): its number in its colour,
// its on/off switch and delete button; its type (a button per type, its shape
// drawn); Freq, Gain (bells, shelves and tilts only) and Q knobs in its colour;
// its slope (cuts and shelves) and placement. Without a band selected, a hint.
// Every change goes through the graph (one undo step per drag); the knobs touch
// their parameter and show its automation, and right-click gives its menu.
Item {
    id: panel

    required property EqGraph graph
    readonly property var band: graph.selectedBand
    readonly property bool shown: band.index !== undefined
    readonly property int index: shown ? band.index : -1
    readonly property color color: shown ? band.color : Theme.accent

    implicitWidth: 162  // PANEL_WIDTH
    implicitHeight: page.implicitHeight

    function bandParam(name) {
        return panel.index >= 0 ? "b" + (panel.index + 1) + "_" + name : ""
    }

    Text {
        anchors.fill: parent
        visible: !panel.shown
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: Theme.textDisabled
        font: Theme.uiFont(8)
        text: qsTr("Click the curve\nto add a band\n\nDouble-click a band\nto switch it off")
    }

    Column {
        id: page
        objectName: "bandControls"
        visible: panel.shown
        width: parent.width
        spacing: 3

        Row {
            width: parent.width
            spacing: 4

            Text {
                width: parent.width - on.width - remove.width - 2 * parent.spacing
                anchors.verticalCenter: parent.verticalCenter
                text: panel.shown ? qsTr("Band %1").arg(panel.index + 1) : ""
                color: panel.color
                font: Theme.uiFont(8, true)
            }
            RoleButton {
                id: on
                objectName: "bandOn"
                width: 14
                height: 14
                role: "activator"
                tooltip: qsTr("Band On/Off")
                checkable: false
                checked: panel.shown && panel.band.on
                onClicked: panel.graph.setBandParam(panel.index, "on", panel.band.on ? 0 : 1, "", "Switch EQ Band")
            }
            RoleButton {
                id: remove
                objectName: "bandDelete"
                width: 16
                height: 14
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                text: "✕"
                font.pointSize: 7
                tooltip: qsTr("Delete the band")
                onClicked: panel.graph.removeBand(panel.graph.selected)
            }
        }

        Row {
            spacing: 1

            Repeater {
                model: panel.graph.types
                T.AbstractButton {
                    id: typeButton
                    required property int index
                    required property string modelData
                    objectName: "type" + index
                    width: 17
                    height: 15
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    onClicked: panel.graph.setType(panel.graph.selected, index)
                    background: EqTypeIcon {
                        kind: typeButton.index
                        color: panel.color
                        checked: panel.shown && panel.band.type === typeButton.index
                        hovered: typeButton.hovered
                        enabled: typeButton.enabled
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: modelData
                }
            }
        }

        Row {
            id: knobs
            width: parent.width
            spacing: 2

            Repeater {
                model: [["freq", qsTr("Freq")], ["gain", qsTr("Gain")], ["q", qsTr("Q")]]
                Column {
                    id: column
                    required property var modelData
                    readonly property string name: modelData[0]
                    readonly property bool usable: name !== "gain" || (panel.shown && panel.graph.hasGain(panel.band.type))
                    width: (knobs.width - 2 * knobs.spacing) / 3
                    spacing: 0

                    DeviceParam {
                        id: parameter
                        session: Session
                        trackId: panel.graph.trackId
                        deviceId: panel.graph.deviceId
                        paramId: panel.bandParam(column.name)
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: column.modelData[1]
                        color: Theme.textDim
                        font: Theme.uiFont(8)
                    }
                    ParamKnob {
                        objectName: "bandKnob_" + column.name
                        anchors.horizontalCenter: parent.horizontalCenter
                        size: 28
                        enabled: column.usable
                        param: parameter
                        color: panel.color
                        bipolar: column.name === "gain"
                        formatter: v => column.name === "freq" ? panel.graph.freqText(v)
                                      : column.name === "gain" ? (v >= 0 ? "+" : "") + v.toFixed(1) + " dB"
                                      : v.toFixed(2)
                        setter: (v, key) => panel.graph.setBandParam(panel.index, column.name, v, key)
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        enabled: column.usable
                        text: {
                            const v = parameter.value
                            return column.name === "freq" ? panel.graph.freqText(v)
                                 : column.name === "gain" ? (v >= 0 ? "+" : "") + v.toFixed(1) + " dB"
                                 : v.toFixed(2)
                        }
                        color: enabled ? Theme.text : Theme.textDisabled
                        font: Theme.uiFont(8)
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: 3

            ComboBox {
                objectName: "bandSlope"
                width: (parent.width - parent.spacing) / 2
                height: 18
                topPadding: 0
                bottomPadding: 0
                font: Theme.uiFont(7)
                focusPolicy: Qt.NoFocus
                enabled: panel.shown && panel.graph.hasSlope(panel.band.type)
                model: panel.graph.slopes
                currentIndex: panel.shown ? panel.band.slope : 0
                onActivated: index => panel.graph.setBandParam(panel.index, "slope", index, "", "Change EQ Band Slope")
                ToolTip.visible: hovered
                ToolTip.delay: 700
                ToolTip.text: qsTr("Slope")
            }
            ComboBox {
                objectName: "bandPlace"
                width: (parent.width - parent.spacing) / 2
                height: 18
                topPadding: 0
                bottomPadding: 0
                font: Theme.uiFont(7)
                focusPolicy: Qt.NoFocus
                model: panel.graph.places
                currentIndex: panel.shown ? panel.band.place : 0
                onActivated: index => panel.graph.setBandParam(panel.index, "place", index, "", "Change EQ Band Placement")
                ToolTip.visible: hovered
                ToolTip.delay: 700
                ToolTip.text: qsTr("Placement: both channels, or only the left, right, mid or side")
            }
        }
    }
}
