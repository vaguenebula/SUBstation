import QtQuick
import QtQuick.Controls
import SUBstation

// The Delay's editor, laid out as Ableton's: each side's time (Sync on: a grid
// of sixteenths and an offset; off: a time knob) and the link between them (the
// right side greyed out while linked); the filter on the echoes (FilterGraph: a
// curve to drag across for its frequency, up and down for its width, over a
// spectrum of the input) with its switch, frequency and width; the mode the
// time changes in and ping pong; and on the right the feedback with its freeze,
// over the dry/wet mix. Every control shows its parameter as it is now (its
// automation's value while that plays), sets it undoably, touches it when
// pressed, and right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property var divisions: ["1", "2", "3", "4", "5", "6", "8", "16"]  // the sixteenths, as the engine lists them
    readonly property int sideWidth: 56
    readonly property int linkWidth: 18
    readonly property int graphWidth: 220
    readonly property int modeWidth: 62
    readonly property int rightWidth: 64
    readonly property int spacing: 10

    // The device's body: 2 * SIDE_WIDTH + LINK_WIDTH + GRAPH_WIDTH + MODE_WIDTH + RIGHT_WIDTH + 4 * SPACING + 18,
    // less the frame's border.
    implicitWidth: 2 * sideWidth + linkWidth + graphWidth + modeWidth + rightWidth + 4 * spacing + 18 - 2
    implicitHeight: 6 + Math.max(leftSide.implicitHeight, rightColumn.implicitHeight, 120) + 6

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
        model: ["l_sync", "l_division", "l_time", "l_offset", "r_sync", "r_division", "r_time", "r_offset", "link",
                "feedback", "freeze", "filter", "freq", "width", "mode", "ping_pong", "mix"]
        DeviceParam {
            required property string modelData
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            paramId: modelData
        }
    }

    component Caption: Text {
        horizontalAlignment: Text.AlignHCenter
        color: Theme.textDim
        font: Theme.uiFont(8)
    }

    component Readout: Text {
        horizontalAlignment: Text.AlignHCenter
        color: enabled ? Theme.text : Theme.textDisabled
        font: Theme.uiFont(8)
    }

    // One side's time: Sync, then the sixteenths and the offset (or, unsynced, the time).
    component Side: Column {
        id: side

        property string prefix: "l"
        property string title: ""
        readonly property bool synced: editor.params[prefix + "_sync"] ? editor.params[prefix + "_sync"].value >= 0.5
                                                                         : true

        width: editor.sideWidth
        spacing: 2

        Caption {
            width: parent.width
            text: side.title
        }
        ParamButton {
            objectName: side.prefix + "Sync"
            width: parent.width
            param: editor.params[side.prefix + "_sync"] || null
            text: qsTr("Sync")
            tooltip: qsTr("Sync the time to the tempo, in sixteenths")
        }
        Item {
            width: parent.width
            height: Math.max(synced.implicitHeight, free.implicitHeight)
            implicitHeight: height

            Grid {
                id: synced
                objectName: side.prefix + "Synced"
                visible: side.synced
                width: parent.width
                columns: 2
                spacing: 2

                Repeater {
                    model: editor.divisions
                    ParamButton {
                        required property string modelData
                        required property int index
                        objectName: side.prefix + "Division" + modelData
                        width: (synced.width - 2) / 2
                        param: editor.params[side.prefix + "_division"] || null
                        choice: index
                        text: modelData
                        tooltip: qsTr("%1 sixteenth%2").arg(modelData).arg(modelData !== "1" ? "s" : "")
                    }
                }
            }
            ParamBox {
                objectName: side.prefix + "Offset"
                visible: side.synced
                y: synced.height + 2
                width: parent.width
                param: editor.params[side.prefix + "_offset"] || null
                step: 0.2
                decimals: 1
                defaultValue: 0.0
                formatter: v => v.toFixed(1) + " %"
                sampleText: "-33.0 %"
                tooltip: qsTr("Offset: lengthens or shortens the synced time (swing)")
            }
            Column {
                id: free
                visible: !side.synced
                width: parent.width
                topPadding: 4
                spacing: 1

                ParamKnob {
                    id: timeKnob
                    objectName: side.prefix + "Time"
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 34
                    param: editor.params[side.prefix + "_time"] || null
                }
                Readout {
                    width: parent.width
                    text: timeKnob.param ? timeKnob.param.text : ""
                }
            }
        }
    }

    Row {
        id: content
        x: 8
        y: 6
        height: editor.height - 12
        spacing: editor.spacing

        Row {
            id: sides
            height: parent.height

            Side {
                id: leftSide
                prefix: "l"
                title: qsTr("Left")
            }
            ParamButton {
                objectName: "link"
                anchors.verticalCenter: parent.verticalCenter
                width: editor.linkWidth
                param: editor.params["link"] || null
                iconName: "link"
                tooltip: qsTr("Link: the right side follows the left")
            }
            Side {
                id: rightSide
                objectName: "rightSide"
                prefix: "r"
                title: qsTr("Right")
                enabled: !(editor.params["link"] && editor.params["link"].value >= 0.5)
            }
        }

        // The filter on the echoes.
        Column {
            width: editor.graphWidth
            height: parent.height
            spacing: 3

            FilterGraph {
                id: graph
                objectName: "filterGraph"
                session: Session
                trackId: editor.trackId
                deviceId: editor.deviceId
                width: parent.width
                height: Math.max(implicitHeight, parent.height - filterRow.height - 3)

                HoverHandler {
                    id: graphHover
                }
                ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
                ToolTip.delay: 700
                ToolTip.text: qsTr("Drag across for the filter's frequency, up and down for its width")
            }
            Row {
                id: filterRow
                width: parent.width
                spacing: 3

                readonly property real boxWidth: (width - filterButton.width - widthLabel.width - 3 * spacing) / 2

                ParamButton {
                    id: filterButton
                    objectName: "filterOn"
                    width: 40
                    anchors.verticalCenter: parent.verticalCenter
                    param: editor.params["filter"] || null
                    text: qsTr("Filter")
                    tooltip: qsTr("The band-pass filter on the echoes")
                }
                ParamBox {
                    objectName: "freq"
                    width: filterRow.boxWidth
                    param: editor.params["freq"] || null
                    logScale: true
                    decimals: 0
                    defaultValue: 1000.0
                    formatter: v => param ? param.format(v) : ""
                    parser: text => param ? param.parse(text) : null
                    sampleText: "18.00 kHz"
                    tooltip: qsTr("Filter frequency")
                }
                Caption {
                    id: widthLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Width")
                }
                ParamBox {
                    objectName: "width"
                    width: filterRow.boxWidth
                    param: editor.params["width"] || null
                    step: 0.05
                    decimals: 2
                    defaultValue: 8.0
                    sampleText: "8.00"
                    tooltip: qsTr("Filter width, in octaves")
                }
            }
        }

        // How a change of time sounds, and ping pong.
        Column {
            width: editor.modeWidth
            spacing: 2

            Caption {
                width: parent.width
                text: qsTr("Mode")
            }
            Repeater {
                model: [[qsTr("Repitch"), qsTr("Repitch: time changes glide, pitching the echoes as a tape delay does")],
                        [qsTr("Fade"), qsTr("Fade: time changes crossfade")],
                        [qsTr("Jump"), qsTr("Jump: time changes switch at once")]]
                ParamButton {
                    required property var modelData
                    required property int index
                    objectName: "mode" + modelData[0]
                    width: parent.width
                    param: editor.params["mode"] || null
                    choice: index
                    text: modelData[0]
                    tooltip: modelData[1]
                }
            }
            Item {
                width: 1
                height: 10
            }
            ParamButton {
                objectName: "pingPong"
                width: parent.width
                param: editor.params["ping_pong"] || null
                text: qsTr("Ping Pong")
                tooltip: qsTr("The echoes bounce from left to right")
            }
        }

        // The feedback, with its freeze beside it, over the dry/wet mix.
        Item {
            id: rightColumn
            width: editor.rightWidth
            height: parent.height
            implicitHeight: top.implicitHeight + bottom.implicitHeight

            Column {
                id: top
                width: parent.width
                spacing: 1

                Caption {
                    width: parent.width
                    text: qsTr("Feedback")
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 2

                    ParamKnob {
                        id: feedback
                        objectName: "feedback"
                        size: 30
                        param: editor.params["feedback"] || null
                    }
                    ParamButton {
                        objectName: "freeze"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        param: editor.params["freeze"] || null
                        iconName: "infinity"
                        tooltip: qsTr("Freeze: what is in the delay goes round for ever, new input is ignored")
                    }
                }
                Readout {
                    width: parent.width
                    text: feedback.param ? feedback.param.text : ""
                }
            }
            Column {
                id: bottom
                anchors.bottom: parent.bottom
                width: parent.width
                spacing: 1

                Caption {
                    width: parent.width
                    text: qsTr("Dry/Wet")
                }
                ParamKnob {
                    id: mix
                    objectName: "mix"
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 30
                    param: editor.params["mix"] || null
                }
                Readout {
                    width: parent.width
                    text: mix.param ? mix.param.text : ""
                }
            }
        }
    }
}
