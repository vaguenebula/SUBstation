import QtQuick
import QtQuick.Controls
import SUBstation

// The Chorus-Ensemble's editor, laid out as Ableton's: the mode tabs (Chorus,
// Ensemble, Vibrato) over the display (ChorusGraph: each voice's delay moving in
// step with the sound, glowing as it passes; drag up and down for the Rate,
// across for the Amount), and under it the high-pass with its frequency and, in
// Chorus mode, Taps and Time; beside it Rate over Amount, Feedback (with Ø, its
// polarity; both dimmed in Vibrato, which has no feedback, but still settable)
// over Warmth, Width (in Vibrato: Offset over Shape), and Output over Dry/Wet.
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click gives
// its menu. Everything with text is as wide as the font makes its text (the
// knobs' cells their widest caption or readout, the box its widest value and
// the automation dot, Time its longest choice), and the editor as wide as its
// parts: 534 px in the default font.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    // The display: 234 px, or as wide as its tabs or its strip need in a wider font.
    readonly property int graphWidth: Math.max(234, 3 * tabWidth + 2 * tabs.spacing, Math.ceil(strip.needed))
    // A knob's cell: as wide as its widest caption or readout (the house's 52 px at least), centred in a
    // column 12 px wider.
    readonly property int cellWidth: Math.max(52, Math.ceil(cellTexts.implicitWidth))
    readonly property int columnWidth: cellWidth + 12
    readonly property int columnSpacing: 2
    // Ø's, between Feedback's column and the next: its text inside the button's border, a pixel clear.
    readonly property int invertWidth: Math.max(14, Math.ceil(invert.button.implicitContentWidth) + 4)
    // A mode tab's least width: the widest tab's own.
    readonly property int tabWidth: Math.ceil(Array.from(tabs.children).reduce(
        (widest, tab) => Math.max(widest, tab.implicitWidth), 0))
    readonly property int spacing: 10
    readonly property int tabHeight: 16
    readonly property int stripHeight: 18
    readonly property real knobHeight: rate.implicitHeight
    // The mode: 0 Chorus, 1 Ensemble, 2 Vibrato.
    readonly property int mode: p.get("mode") ? p.get("mode").index : 0
    readonly property bool highPassOn: p.get("hp") ? p.get("hp").value >= 0.5 : false
    // A control that does nothing now (in this mode, or with its switch off) but can still be set: dimmed.
    readonly property real dim: 0.55
    readonly property var tapLabels: ["1", "2"]  // the Taps buttons'

    // The cross-fades between what one mode shows and another's (the strip's Taps and Time or its voices,
    // column 3's Width or Offset and Shape): 1 for the first, 0 for the second, eased. The old set fades out
    // over the first half and the new in over the second, never both at once, so their texts never overlap.
    property real chorusShown: mode === 0 ? 1 : 0
    property real widthShown: mode !== 2 ? 1 : 0
    Behavior on chorusShown {
        NumberAnimation {
            duration: 140
            easing.type: Easing.InOutQuad
        }
    }
    Behavior on widthShown {
        NumberAnimation {
            duration: 140
            easing.type: Easing.InOutQuad
        }
    }

    // The device's body: 8 + GRAPH_WIDTH + SPACING + 4 * COLUMN_WIDTH + INVERT_WIDTH + 3 * COLUMN_SPACING + 8,
    // less the frame's border.
    implicitWidth: 8 + graphWidth + spacing + 4 * columnWidth + invertWidth + 3 * columnSpacing + 8 - 2
    implicitHeight: 6 + Math.max(2 * knobHeight + 6, tabHeight + 3 + graph.implicitHeight + 3 + stripHeight) + 6

    // The strip's text outside Chorus mode: the engine's voices a side.
    function voicesLabel(voices) {
        return voices === 1 ? qsTr("1 voice a side") : qsTr("%1 voices a side").arg(voices)
    }

    // The widest of `lines` as a Text lays them out in its font (as a caption, a readout or a button's text is
    // as wide as its advance and whatever of its last glyph reaches past it): hidden, a line each.
    component Widest: Text {
        property var lines: []
        visible: false
        textFormat: Text.PlainText
        text: lines.join("\n")
    }
    // What the knobs' cells show at their widest: their captions, and each readout's widest form (Rate's
    // "##.## Hz", for 9.996 reads "10.00 Hz"; the percentages, up to Width's 200; Offset's degrees; Output's
    // "-##.# dB"), every figure the font's widest (figures may be proportional): no value is cut short, whatever
    // the font. In the captions' and readouts' font.
    Widest {
        id: cellTexts
        font: Theme.uiFont(8)
        lines: [rate, amountKnob, feedback, warmthKnob, widthKnob, offsetKnob, shapeKnob, outputKnob, mixKnob]
            .map(knob => knob.title)
            .concat(["##.## Hz", "### %", "###°", "-##.# dB"].map(pattern => figures.sample(pattern)))
    }
    // Time's choices and the Taps buttons' figures, in the buttons' font.
    Widest {
        id: timeTexts
        font: time.button.font
        lines: time.names
    }
    Widest {
        id: tapTexts
        font: time.button.font
        lines: editor.tapLabels
    }
    // The strip's voices, outside Chorus mode.
    Widest {
        id: voicesTexts
        font: Theme.uiFont(8)
        lines: [1, 2].map(other => editor.voicesLabel(graph.sideVoices(other)))
    }
    // The captions' and readouts' figures (the value box's too).
    FontMetrics {
        id: figures

        // The widest figure (figures may be proportional; read in a binding, the font is followed).
        readonly property string widest: {
            void font
            let widest = "0"
            for (const digit of "123456789") {
                if (advanceWidth(digit) > advanceWidth(widest))
                    widest = digit
            }
            return widest
        }

        // `pattern` with each "#" the widest figure: at least as wide as any value of that form.
        function sample(pattern) {
            return pattern.replace(/#/g, widest)
        }
        // Of `texts`, the one with the widest advance (the font followed as above).
        function widestOf(texts) {
            void font
            return texts.reduce((most, text) => advanceWidth(text) > advanceWidth(most) ? text : most, "")
        }
        function advanceOf(text) {
            void font
            return advanceWidth(text)
        }

        font: Theme.uiFont(8)
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["mode", "taps", "time", "rate", "amount", "feedback", "fb_invert", "width", "offset", "shape", "warmth",
              "hp", "hp_freq", "output", "mix"]
    }

    // How many voices a side the strip names outside Chorus mode, the engine's: set as the mode changes rather
    // than bound, so that going back to Chorus it keeps naming the old mode's while it fades out.
    onModeChanged: if (mode !== 0) voicesText.voices = graph.sideVoices(mode)
    Component.onCompleted: voicesText.voices = graph.sideVoices(mode)

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
                    // A third of the display each, on whole pixels.
                    readonly property real share: (editor.graphWidth - 2 * tabs.spacing) / 3
                    objectName: "mode" + modelData[0]
                    width: Math.round((index + 1) * share) - Math.round(index * share)
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
            ToolTip.text: qsTr("Each voice's delay as it moves, in step with the sound (orange left, blue right). "
                               + "Drag up and down for the Rate, across for the Amount")
        }

        Item {
            id: strip

            // The width its parts need: the switch and the box at the left, 8 px apart from the widest of
            // what stands at its right (Taps and Time, or the voices).
            readonly property real needed: hpFreq.x + hpFreq.width + 8
                                           + Math.max(chorusOptions.needed, voicesTexts.implicitWidth)

            y: parent.height - height
            width: parent.width
            height: editor.stripHeight

            ParamButton {
                objectName: "hp"
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                param: p.get("hp")
                iconName: "filter_highpass"
                tooltip: qsTr("High-pass: below this frequency the sound isn't chorused "
                              + "(the lows pass through unmodulated)")
            }
            // As wide as its widest value, centred 9 px from either edge: clear of the automation dot (6 px in,
            // 2.5 px round). Its values: "20 Hz" to "1000 Hz" (999.6 rounded), "1.00 kHz" to "2.00 kHz".
            ParamBox {
                id: hpFreq
                objectName: "hpFreq"
                x: 25
                width: Math.ceil(figures.advanceOf(sampleText)) + 2 * 9
                param: p.get("hp_freq")
                logScale: true
                decimals: 0
                defaultValue: 100.0
                formatter: v => param ? param.format(v) : ""
                parser: text => param ? param.parse(text) : null
                sampleText: figures.widestOf([figures.sample("#### Hz"), figures.sample("1.## kHz"), "2.00 kHz"])
                tooltip: qsTr("High-pass: below this frequency the sound isn't chorused "
                              + "(the lows pass through unmodulated)")
                opacity: editor.highPassOn ? 1 : editor.dim
                Behavior on opacity {
                    NumberAnimation {
                        duration: 120
                    }
                }
            }

            // Chorus mode's Taps and Time, at the strip's right end: Time as wide as its longest choice with the
            // arrow, the Taps buttons (16 px, or their figure and a pixel clear inside the border) 4 px before it,
            // their caption 4 px before them.
            Item {
                id: chorusOptions
                anchors.fill: parent
                opacity: Math.max(0, 2 * editor.chorusShown - 1)
                visible: opacity > 0

                readonly property int tapWidth: Math.max(16, Math.ceil(tapTexts.implicitWidth) + 4)
                readonly property real tapsX: time.x - 4 - (2 * tapWidth + 2)  // the first Taps button
                readonly property real needed: tapsCaption.implicitWidth + 4 + 2 * tapWidth + 2 + 4 + time.width

                EditorCaption {
                    id: tapsCaption
                    objectName: "tapsCaption"
                    x: chorusOptions.tapsX - 4 - width
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Taps")
                }
                Repeater {
                    model: editor.tapLabels
                    ParamButton {
                        required property string modelData
                        required property int index
                        objectName: "taps" + modelData
                        x: chorusOptions.tapsX + (chorusOptions.tapWidth + 2) * index
                        anchors.verticalCenter: parent.verticalCenter
                        width: chorusOptions.tapWidth
                        param: p.get("taps")
                        choice: index
                        text: modelData
                        tooltip: qsTr("Taps: one modulated delay a side (simpler and thicker, as a pedal) "
                                      + "or two moving opposite ways")
                    }
                }
                ParamChoice {
                    id: time
                    objectName: "time"
                    x: parent.width - width
                    width: Math.ceil(timeTexts.implicitWidth) + button.leftPadding + button.rightPadding
                    anchors.verticalCenter: parent.verticalCenter
                    param: p.get("time")
                    tooltip: qsTr("Time: the delays' length. Auto follows the Amount (the classic chorus); "
                                  + "a fixed time holds still, for basses and guitars")
                }
            }
            // Ensemble's and Vibrato's voices instead.
            EditorCaption {
                id: voicesText
                objectName: "voicesText"

                property int voices  // (set by the editor)

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: editor.voicesLabel(voices)
                opacity: Math.max(0, 1 - 2 * editor.chorusShown)
                visible: opacity > 0
            }
        }
    }

    // --- The knobs: four columns of two rows ---------------------------------------------------------

    // A knob in its cell (the editor's), its caption over it and its readout under it.
    component CellKnob: EditorKnob {
        width: editor.cellWidth
    }

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

        CellKnob {
            id: rate
            objectName: "rate"
            x: knobs.column(0) + knobs.inset
            param: p.get("rate")
            title: qsTr("Rate")
            tooltip: qsTr("Rate: how fast the delays move, in Hz. Also drag up and down in the display")
        }
        CellKnob {
            id: amountKnob
            objectName: "amount"
            x: knobs.column(0) + knobs.inset
            y: knobs.bottomRow
            param: p.get("amount")
            title: qsTr("Amount")
            tooltip: qsTr("Amount: how far the delays move (the depth of the modulation)")
        }

        CellKnob {
            id: feedback
            objectName: "feedback"
            x: knobs.column(1) + knobs.inset
            opacity: editor.mode !== 2 ? 1 : editor.dim
            param: p.get("feedback")
            title: qsTr("Feedback")
            tooltip: qsTr("Feedback: how much of each side's output goes back into its delays (not in Vibrato)")
        }
        ParamButton {
            id: invert
            objectName: "fbInvert"
            x: feedback.x + feedback.width
            y: feedback.y + feedback.knob.y + feedback.knob.height / 2 - height / 2
            width: editor.invertWidth
            opacity: editor.mode !== 2 ? 1 : editor.dim
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
            param: p.get("fb_invert")
            text: "Ø"
            tooltip: qsTr("Invert the feedback's polarity: hollow at high Feedback (not in Vibrato)")
        }
        CellKnob {
            id: warmthKnob
            objectName: "warmth"
            x: knobs.column(1) + knobs.inset
            y: knobs.bottomRow
            param: p.get("warmth")
            title: qsTr("Warmth")
            tooltip: qsTr("Warmth: a subtle distortion and darkening of the chorused sound "
                          + "(a distortion of its own at Amount 0)")
        }

        // The mode's column: Width, or in Vibrato Offset over Shape.
        Item {
            x: knobs.column(2)
            width: editor.columnWidth
            height: knobs.height
            opacity: Math.max(0, 2 * editor.widthShown - 1)
            visible: opacity > 0

            CellKnob {
                id: widthKnob
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
            opacity: Math.max(0, 1 - 2 * editor.widthShown)
            visible: opacity > 0

            CellKnob {
                id: offsetKnob
                objectName: "offset"
                x: knobs.inset
                param: p.get("offset")
                title: qsTr("Offset")
                tooltip: qsTr("Offset: how far apart the left and right vibrato move (180°: opposite)")
            }
            CellKnob {
                id: shapeKnob
                objectName: "shape"
                x: knobs.inset
                y: knobs.bottomRow
                param: p.get("shape")
                title: qsTr("Shape")
                tooltip: qsTr("Shape: the vibrato's wave, from a sine (0 %) to a triangle (100 %)")
            }
        }

        CellKnob {
            id: outputKnob
            objectName: "output"
            x: knobs.column(3) + knobs.inset
            param: p.get("output")
            title: qsTr("Output")
            tooltip: qsTr("Output: the level of the chorused sound")
        }
        CellKnob {
            id: mixKnob
            objectName: "mix"
            x: knobs.column(3) + knobs.inset
            y: knobs.bottomRow
            param: p.get("mix")
            title: qsTr("Dry/Wet")
            tooltip: qsTr("Dry/Wet: the dry sound blended with the chorused")
        }
    }
}
