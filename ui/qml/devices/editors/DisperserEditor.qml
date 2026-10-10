import QtQuick
import QtQuick.Controls
import SUBstation

// The Disperser's editor: Amount (the number of stages), Frequency, Pinch and
// Dry/Wet knobs side by side over the Bypass button, and beside them the group delay
// the stages give each frequency (DispersionGraph, in ms, worked out from the
// engine's own stages; drag its dot across for the frequency, up and down for
// the pinch). Every control shows its parameter as it is now (its automation's
// value while that plays), sets it undoably, touches it when pressed, and
// right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property int knobWidth: 62
    readonly property int graphWidth: 260
    readonly property int spacing: 10

    // The device's body: 8 + 4 * KNOB_WIDTH + 3 * 4 + SPACING + GRAPH_WIDTH + 8, less the frame's border.
    implicitWidth: 8 + 4 * knobWidth + 3 * 4 + spacing + graphWidth + 8 - 2
    implicitHeight: 6 + Math.max(controls.implicitHeight, graph.implicitHeight) + 6

    // The parameters, by id.
    readonly property var params: {
        const all = {}
        for (let i = 0; i < paramObjects.count; ++i) {
            const p = paramObjects.objectAt(i)
            if (p)
                all[p.paramId] = p
        }
        return all
    }
    Instantiator {
        id: paramObjects
        model: ["amount", "freq", "pinch", "mix", "bypass"]
        DeviceParam {
            required property string modelData
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            paramId: modelData
        }
    }

    // A knob with its name above and its value below.
    component LabelledKnob: Column {
        id: cell

        property string paramId: ""
        property string title: ""
        property real step: 0
        readonly property alias knob: knob

        width: editor.knobWidth
        spacing: 1

        EditorCaption {
            width: parent.width
            text: cell.title
        }
        ParamKnob {
            id: knob
            objectName: cell.paramId
            anchors.horizontalCenter: parent.horizontalCenter
            size: 34
            step: cell.step
            param: editor.params[cell.paramId] || null
        }
        EditorReadout {
            width: parent.width
            text: knob.param ? knob.param.text : ""
        }
    }

    Row {
        x: 8
        y: 6
        height: editor.height - 12
        spacing: editor.spacing

        Column {
            id: controls
            spacing: 6

            Row {
                spacing: 4

                LabelledKnob {
                    paramId: "amount"
                    title: qsTr("Amount")
                    step: 1  // whole stages
                    ToolTip.visible: amountHover.hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Amount: how many all-pass stages the sound goes through (0: none)")
                    HoverHandler {
                        id: amountHover
                    }
                }
                LabelledKnob {
                    paramId: "freq"
                    title: qsTr("Frequency")
                    ToolTip.visible: freqHover.hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Frequency: where the stages delay the sound most")
                    HoverHandler {
                        id: freqHover
                    }
                }
                LabelledKnob {
                    paramId: "pinch"
                    title: qsTr("Pinch")
                    ToolTip.visible: pinchHover.hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Pinch: how narrow the band they delay (their Q); narrower, it is delayed longer")
                    HoverHandler {
                        id: pinchHover
                    }
                }
                LabelledKnob {
                    paramId: "mix"
                    title: qsTr("Dry/Wet")
                    ToolTip.visible: mixHover.hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Dry/Wet: the input blended with the dispersed sound; in between they add up as a phaser's do")
                    HoverHandler {
                        id: mixHover
                    }
                }
            }
            ParamButton {
                objectName: "bypass"
                anchors.horizontalCenter: parent.horizontalCenter
                width: 4 * editor.knobWidth + 3 * 4
                param: editor.params["bypass"] || null
                text: qsTr("Bypass")
                tooltip: qsTr("Bypass: the sound passes through untouched (the stages keep running, so it comes back without a seam)")
            }
        }

        DispersionGraph {
            id: graph
            objectName: "dispersionGraph"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            width: editor.graphWidth
            height: Math.max(implicitHeight, parent.height)

            HoverHandler {
                id: graphHover
            }
            ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
            ToolTip.delay: 700
            ToolTip.text: qsTr("How late each frequency comes out. Drag across for the frequency, up and down for the pinch")
        }
    }
}
