import QtQuick
import QtQuick.Controls
import SUBstation

// The Sidechain device's editor: its curve, big, to draw on (CurveGraph), from
// edge to edge; then what the kick and the input clash at (ClashView) with Fit,
// Auto and the fit's character; then the device's controls: the trigger and
// Sync, six small knobs (the length in ms, or synced in notes), Lows Only over
// the crossover. The hint over the curve (no sidechain chosen while triggered
// by one) asks the device's frame for its sidechain menu
// (sidechainMenuRequested).
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    readonly property alias clash: clash
    readonly property alias curveMenu: curveMenu

    // The hint over the curve was clicked: the device's frame shows its sidechain menu.
    signal sidechainMenuRequested()

    // The device's body: GRAPH_MIN_WIDTH + 2 * SPACING + FIT_WIDTH + CONTROLS_WIDTH + INSET, from edge to edge.
    implicitWidth: 380 + 2 * 8 + 150 + 168 + 6
    implicitHeight: Math.max(100, 5 + controls.implicitHeight + 5)

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
        model: ["trigger", "threshold", "depth", "sync", "length", "rate", "smooth", "lookahead", "range", "crossover",
                "autofit", "character"]
        DeviceParam {
            required property string modelData
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            paramId: modelData
        }
    }

    // A small knob with its name over it (or, over the crossover, Lows Only) and its value under it.
    component SmallKnob: Column {
        id: cell

        property string paramId
        property string title
        property bool lowsOnly: false
        property var formatter: null
        property real stepSize: 0
        property bool usable: true
        readonly property DeviceParam param: editor.params[paramId] || null

        width: controls.width / 3
        spacing: 0

        Loader {
            anchors.horizontalCenter: parent.horizontalCenter
            sourceComponent: cell.lowsOnly ? lowsOnly : caption

            Component {
                id: caption
                EditorCaption {
                    width: cell.width
                    text: cell.title
                    font: Theme.uiFont(7)
                }
            }
            Component {
                id: lowsOnly
                ParamButton {
                    objectName: "lowsOnly"
                    height: 13
                    param: editor.params["range"] || null
                    text: qsTr("Lows Only")
                    font.pointSize: 7
                    tooltip: qsTr("Duck only what is below the crossover")
                    setter: value => graph.setParamValue("range", value, "", "Change Range")
                    Component.onCompleted: {
                        button.leftPadding = 2
                        button.rightPadding = 2
                    }
                }
            }
        }
        ParamKnob {
            id: knob
            objectName: "knob_" + cell.paramId
            anchors.horizontalCenter: parent.horizontalCenter
            size: 26
            enabled: cell.usable
            param: cell.param
            step: cell.stepSize
            formatter: v => cell.formatter ? cell.formatter(v) : (cell.param ? cell.param.format(v) : "")
            setter: (v, key) => graph.setParamValue(cell.paramId, cell.stepSize ? Math.round(v) : v, key,
                                                    "Change " + cell.title)
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            enabled: cell.usable
            text: cell.param ? (cell.formatter ? cell.formatter(cell.param.value) : cell.param.text) : ""
            color: enabled ? Theme.text : Theme.textDisabled
            font: Theme.uiFont(7)
        }
    }

    Row {
        anchors.fill: parent
        anchors.rightMargin: 6  // INSET
        spacing: 8

        CurveGraph {
            id: graph
            objectName: "curveGraph"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            width: parent.width - 2 * parent.spacing - fit.width - controls.width
            height: parent.height
            onSidechainMenuRequested: editor.sidechainMenuRequested()
            onContextMenuRequested: (position, point) => curveMenu.showAt(position, point)
        }

        // The clash, and fitting the curve to the kick there.
        Column {
            id: fit
            width: 150
            height: parent.height - 5 - 5
            y: 5
            spacing: 4

            ClashView {
                id: clash
                objectName: "clashView"
                graph: graph
                width: parent.width
                height: Math.max(implicitHeight, parent.height - fitButtons.height - character.height - 2 * parent.spacing)

                HoverHandler {
                    id: clashHover
                }
                ToolTip.visible: clashHover.hovered
                ToolTip.delay: 700
                ToolTip.text: qsTr("Where the kick (orange) and the input (blue) clash (pink).\nClick to fit the curve to the kick there.")
            }
            Row {
                id: fitButtons
                width: parent.width
                spacing: 4

                RoleButton {
                    objectName: "fit"
                    width: (parent.width - parent.spacing) / 2
                    height: 18
                    role: "small"
                    text: qsTr("Fit")
                    tooltip: qsTr("Fit the curve to the kick, where it clashes with the input")
                    onClicked: graph.fitNow()
                }
                ParamButton {
                    objectName: "auto"
                    width: (parent.width - parent.spacing) / 2
                    height: 18
                    param: editor.params["autofit"] || null
                    text: qsTr("Auto")
                    tooltip: qsTr("Fit again at every hit")
                    setter: value => graph.setAuto(value >= 0.5)
                }
            }
            ComboBox {
                id: character
                objectName: "character"
                width: parent.width
                focusPolicy: Qt.NoFocus
                model: [qsTr("Tight"), qsTr("Natural"), qsTr("Loose")]
                currentIndex: editor.params["character"] ? editor.params["character"].index : 1
                onActivated: index => graph.setCharacter(index)
                ToolTip.visible: hovered
                ToolTip.delay: 700
                ToolTip.text: qsTr("How long the fit keeps the input out of the kick's way:\nTight while it is loud, Loose as long as it lingers")
            }
        }

        // The device's controls.
        Column {
            id: controls
            width: 168
            y: 5
            spacing: 2

            Row {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width - syncButton.width - parent.spacing
                    height: trigger.implicitHeight

                    ParamArea {
                        anchors.fill: parent
                        param: editor.params["trigger"] || null
                    }
                    ComboBox {
                        id: trigger
                        objectName: "trigger"
                        anchors.fill: parent
                        focusPolicy: Qt.NoFocus
                        model: editor.params["trigger"] ? editor.params["trigger"].labels : []
                        currentIndex: editor.params["trigger"] ? editor.params["trigger"].index : 0
                        onActivated: index => graph.setParamValue("trigger", index, "", "Change Trigger")
                        onPressedChanged: if (pressed && editor.params["trigger"]) editor.params["trigger"].touch()
                        ToolTip.visible: hovered
                        ToolTip.delay: 700
                        ToolTip.text: qsTr("What starts the curve: a hit in the sidechain, or the beat")
                    }
                }
                ParamButton {
                    id: syncButton
                    objectName: "sync"
                    width: 40
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    param: editor.params["sync"] || null
                    text: qsTr("Sync")
                    tooltip: qsTr("The length in notes, at the tempo")
                    setter: value => graph.setParamValue("sync", value, "", "Change Sync")
                }
            }
            Item {
                width: 1
                height: 2
            }
            Grid {
                columns: 3
                rowSpacing: 1
                columnSpacing: 0

                SmallKnob {
                    paramId: "threshold"
                    title: qsTr("Threshold")
                }
                SmallKnob {
                    paramId: "depth"
                    title: qsTr("Depth")
                }
                Item {
                    objectName: "lengthStack"
                    width: lengthKnob.width
                    height: Math.max(lengthKnob.implicitHeight, rateKnob.implicitHeight)
                    implicitHeight: height

                    SmallKnob {
                        id: lengthKnob
                        visible: !(editor.params["sync"] && editor.params["sync"].value >= 0.5)
                        paramId: "length"
                        title: qsTr("Length")
                    }
                    SmallKnob {
                        id: rateKnob
                        visible: !lengthKnob.visible
                        paramId: "rate"
                        title: qsTr("Length")
                        stepSize: 1
                        formatter: v => {
                            const labels = rateKnob.param ? rateKnob.param.labels : []
                            return labels[Math.max(0, Math.min(labels.length - 1, Math.round(v)))] || ""
                        }
                    }
                }
                SmallKnob {
                    paramId: "smooth"
                    title: qsTr("Smooth")
                }
                SmallKnob {
                    paramId: "lookahead"
                    title: qsTr("Lookahead")
                }
                SmallKnob {
                    paramId: "crossover"
                    title: qsTr("Crossover")
                    lowsOnly: true
                    usable: editor.params["range"] ? editor.params["range"].value >= 0.5 : false
                }
            }
        }
    }

    // The curve's right-click menu: delete the point, shapes to start from, fit, flip, reset.
    DynamicMenu {
        id: curveMenu

        function build(point) {
            clear()
            if (point >= 0) {
                entry(qsTr("Delete Point"), () => graph.removePoint(point))
                separator()
            }
            const shapes = submenu(qsTr("Shapes"))
            for (let i = 0; i < graph.shapes.length; ++i) {
                const index = i
                entry(graph.shapes[i], () => graph.applyShape(index), undefined, true, shapes)
            }
            entry(qsTr("Fit to Kick"), () => graph.fitNow(), undefined, graph.hasFit)
            entry(qsTr("Flip Curve"), () => graph.flip())
            separator()
            entry(qsTr("Reset Curve"), () => graph.resetCurve())
        }

        function showAt(position, point) {
            build(point)
            popup(graph, position.x, position.y)
        }
    }
}
