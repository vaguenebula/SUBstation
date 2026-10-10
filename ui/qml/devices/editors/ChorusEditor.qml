import QtQuick
import QtQuick.Controls
import SUBstation

// The Chorus-Ensemble's editor, laid out as Ableton's: the mode tabs (Chorus,
// Ensemble, Vibrato) over the display (ChorusGraph: each voice's delay moving in
// step with the sound, glowing as it passes; drag up and down for the Rate,
// across for the Amount), and under it the high-pass with its frequency and, in
// Chorus mode, Taps and Time; beside it Rate over Amount, Feedback (with Ø, its
// polarity) over Warmth, Width (in Vibrato: Offset over Shape), and Output over
// Dry/Wet. Every control shows its parameter as it is now (its automation's
// value while that plays), sets it undoably, touches it when pressed, and
// right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property int graphWidth: 234
    readonly property int columnWidth: 64
    readonly property int columnSpacing: 2
    readonly property int invertWidth: 14  // Ø, between Feedback's column and the next
    readonly property int spacing: 10
    readonly property int tabHeight: 16
    readonly property int stripHeight: 18
    readonly property real knobHeight: rate.implicitHeight
    // The mode: 0 Chorus, 1 Ensemble, 2 Vibrato.
    readonly property int mode: p.get("mode") ? p.get("mode").index : 0
    readonly property bool highPassOn: p.get("hp") ? p.get("hp").value >= 0.5 : false

    // The device's body: 8 + GRAPH_WIDTH + SPACING + 4 * COLUMN_WIDTH + INVERT_WIDTH + 3 * COLUMN_SPACING + 8,
    // less the frame's border.
    implicitWidth: 8 + graphWidth + spacing + 4 * columnWidth + invertWidth + 3 * columnSpacing + 8 - 2
    implicitHeight: 6 + Math.max(2 * knobHeight + 6, tabHeight + 3 + graph.implicitHeight + 3 + stripHeight) + 6

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["mode", "taps", "time", "rate", "amount", "feedback", "invert", "width", "offset", "shape", "warmth", "hp",
              "hp_freq", "output", "mix"]
    }

    // How many voices a side the strip names outside Chorus mode (kept while it fades out).
    onModeChanged: if (mode !== 0) voicesText.voices = mode === 1 ? 3 : 1

    // --- The display: the mode tabs, the graph, the high-pass strip --------------------------------

    Item {
        id: display
        x: 8
        y: 6
        width: editor.graphWidth
        height: editor.height - 12

        Row {
            id: tabs
            spacing: 3

            Repeater {
                model: [["Chorus", qsTr("Chorus"),
                         qsTr("Chorus: one or two modulated delays a side added to the sound: the classic chorus")],
                        ["Ensemble", qsTr("Ensemble"),
                         qsTr("Ensemble: three delays a side, their modulation 120° apart: thicker and smoother")],
                        ["Vibrato", qsTr("Vibrato"),
                         qsTr("Vibrato: one delay a side: its pitch moves; fully wet, a pure vibrato")]]
                ParamButton {
                    required property var modelData
                    required property int index
                    objectName: "mode" + modelData[0]
                    width: (editor.graphWidth - 2 * tabs.spacing) / 3
                    height: editor.tabHeight
                    param: p.get("mode")
                    choice: index
                    text: modelData[1]
                    tooltip: modelData[2]
                }
            }
        }

        ChorusGraph {
            id: graph
            objectName: "chorusGraph"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            y: editor.tabHeight + 3
            width: parent.width
            height: Math.max(implicitHeight, parent.height - editor.tabHeight - 3 - 3 - editor.stripHeight)

            HoverHandler {
                id: graphHover
            }
            ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
            ToolTip.delay: 700
            ToolTip.text: qsTr("Each voice's delay as it moves, in step with the sound (orange left, blue right). Drag up and down for the Rate, across for the Amount")
        }

        Item {
            id: strip
            y: parent.height - height
            width: parent.width
            height: editor.stripHeight

            ParamButton {
                objectName: "hp"
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                param: p.get("hp")
                iconName: "filter_highpass"
                tooltip: qsTr("High-pass: below this frequency the sound isn't chorused (the lows pass through unmodulated)")
            }
            ParamBox {
                objectName: "hpFreq"
                x: 25
                width: 54
                param: p.get("hp_freq")
                logScale: true
                decimals: 0
                defaultValue: 100.0
                formatter: v => param ? param.format(v) : ""
                parser: text => param ? param.parse(text) : null
                sampleText: "2.00 kHz"
                tooltip: qsTr("High-pass: below this frequency the sound isn't chorused (the lows pass through unmodulated)")
                opacity: editor.highPassOn ? 1 : 0.5
                Behavior on opacity {
                    NumberAnimation {
                        duration: 120
                    }
                }
            }

            // Chorus mode's Taps and Time.
            Item {
                id: chorusOptions
                anchors.fill: parent
                opacity: editor.mode === 0 ? 1 : 0
                visible: opacity > 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 120
                    }
                }

                EditorCaption {
                    x: 112
                    width: 24
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Taps")
                }
                Repeater {
                    model: ["1", "2"]
                    ParamButton {
                        required property string modelData
                        required property int index
                        objectName: "taps" + modelData
                        x: 138 + 18 * index
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        param: p.get("taps")
                        choice: index
                        text: modelData
                        tooltip: qsTr("Taps: one modulated delay a side (simpler and thicker, as a pedal) or two moving opposite ways")
                    }
                }
                ParamChoice {
                    objectName: "time"
                    x: 176
                    width: parent.width - x
                    anchors.verticalCenter: parent.verticalCenter
                    param: p.get("time")
                    tooltip: qsTr("Time: the delays' length. Auto follows the Amount (the classic chorus); a fixed time holds still, for basses and guitars")
                }
            }
            // Ensemble's and Vibrato's voices instead.
            EditorCaption {
                id: voicesText
                objectName: "voicesText"

                property int voices: editor.mode === 2 ? 1 : 3

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: voices === 1 ? qsTr("1 voice a side") : qsTr("%1 voices a side").arg(voices)
                opacity: editor.mode !== 0 ? 1 : 0
                visible: opacity > 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 120
                    }
                }
            }
        }
    }

    // --- The knobs: four columns of two rows ---------------------------------------------------------

    Item {
        id: knobs
        x: display.x + display.width + editor.spacing
        y: 6
        width: 4 * editor.columnWidth + editor.invertWidth + 3 * editor.columnSpacing
        height: editor.height - 12

        readonly property real bottomRow: height - editor.knobHeight
        // Column i's left edge; a knob is centred in it. Feedback's column (1) is wider by Ø.
        function column(i) {
            return i * (editor.columnWidth + editor.columnSpacing) + (i > 1 ? editor.invertWidth : 0)
        }
        readonly property real inset: (editor.columnWidth - rate.width) / 2

        EditorKnob {
            id: rate
            objectName: "rate"
            x: knobs.column(0) + knobs.inset
            param: p.get("rate")
            title: qsTr("Rate")
            tooltip: qsTr("Rate: how fast the delays move, in Hz. Also drag up and down in the display")
        }
        EditorKnob {
            objectName: "amount"
            x: knobs.column(0) + knobs.inset
            y: knobs.bottomRow
            param: p.get("amount")
            title: qsTr("Amount")
            tooltip: qsTr("Amount: how far the delays move (the depth of the modulation)")
        }

        EditorKnob {
            id: feedback
            objectName: "feedback"
            x: knobs.column(1) + knobs.inset
            param: p.get("feedback")
            title: qsTr("Feedback")
            tooltip: qsTr("Feedback: how much of each side's output goes back into its delays")
        }
        ParamButton {
            objectName: "invert"
            x: feedback.x + feedback.width
            y: feedback.y + feedback.knob.y + feedback.knob.height / 2 - height / 2
            width: editor.invertWidth
            param: p.get("invert")
            text: "Ø"
            tooltip: qsTr("Invert the feedback's polarity: hollow at high Feedback (not in Vibrato)")
            enabled: editor.mode !== 2
        }
        EditorKnob {
            objectName: "warmth"
            x: knobs.column(1) + knobs.inset
            y: knobs.bottomRow
            param: p.get("warmth")
            title: qsTr("Warmth")
            tooltip: qsTr("Warmth: a subtle distortion and darkening of the chorused sound (a distortion of its own at Amount 0)")
        }

        // The mode's column: Width, or in Vibrato Offset over Shape.
        Item {
            x: knobs.column(2)
            width: editor.columnWidth
            height: knobs.height
            opacity: editor.mode !== 2 ? 1 : 0
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            EditorKnob {
                objectName: "width"
                x: knobs.inset
                y: (parent.height - editor.knobHeight) / 2
                param: p.get("width")
                title: qsTr("Width")
                tooltip: qsTr("Width: the chorused sound's stereo width: 0 % mono, 100 % as it is, 200 % wider")
            }
        }
        Item {
            x: knobs.column(2)
            width: editor.columnWidth
            height: knobs.height
            opacity: editor.mode === 2 ? 1 : 0
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            EditorKnob {
                objectName: "offset"
                x: knobs.inset
                param: p.get("offset")
                title: qsTr("Offset")
                tooltip: qsTr("Offset: how far apart the left and right vibrato move (180°: opposite)")
            }
            EditorKnob {
                objectName: "shape"
                x: knobs.inset
                y: knobs.bottomRow
                param: p.get("shape")
                title: qsTr("Shape")
                tooltip: qsTr("Shape: the vibrato's wave, from a sine (0 %) to a triangle (100 %)")
            }
        }

        EditorKnob {
            objectName: "output"
            x: knobs.column(3) + knobs.inset
            param: p.get("output")
            title: qsTr("Output")
            tooltip: qsTr("Output: the level of the chorused sound")
        }
        EditorKnob {
            objectName: "mix"
            x: knobs.column(3) + knobs.inset
            y: knobs.bottomRow
            param: p.get("mix")
            title: qsTr("Dry/Wet")
            tooltip: qsTr("Dry/Wet: the dry sound blended with the chorused")
        }
    }
}
