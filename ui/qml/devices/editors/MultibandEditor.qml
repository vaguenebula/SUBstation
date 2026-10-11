import QtQuick
import QtQuick.Controls
import SUBstation

// Multiband Dynamics' editor, laid out as Ableton's: a row per band, High on
// top (its activator, Live's band button, and solo; under High's and Low's the
// crossover, with the switch that splits that band off; the band's Input; its
// lane of the display; two fields the T/B/A buttons switch between its Time
// (attack, release), Below and Above (threshold, ratio); its Output), and at
// the right the device's Amount, Time and Output, Soft Knee, Peak/RMS and the
// sidechain's Gain, Mix, button and Listen. The display (MultibandGraph) shows
// each band's level before and after its dynamics and its two regions: drag a
// region's edge for its threshold, inside it up or down for its ratio. A band
// switched off (its split: the Mid band then shapes its frequencies) or
// bypassed (its activator) dims its controls; they stay editable, as Live
// keeps them. The columns are as wide as their texts need in the font the UI
// has (measured): every box its texts with the automation dot clear of them,
// every caption its name, every knob its name and every text its readout
// takes, every button its text.
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click
// gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    // Which fields the bands show: "T" (attack, release), "B" (Below) or "A" (Above). View state: kept by
    // device (DeviceViews), so it outlives the frames being made again.
    readonly property string fieldsPage: DeviceViews.value(deviceId, "page", "A")
    // The rows, as the graph's lanes: High, Mid, Low under a 16 px header.
    readonly property real rowHeight: graph.rowHeight
    readonly property int contentTop: 22
    // A control that can do something only later (its band switched off or bypassed, no sidechain):
    // dimmed as Live greys it, still settable.
    readonly property real dimmed: 0.55

    // The sidechain's menu, under the item `from` (the Sidechain button).
    signal sidechainMenuRequested(var from)

    implicitWidth: globalX + globals.width + 8
    // At the least: the header and three rows of 38 px, or the device's controls 2 px apart.
    implicitHeight: Math.max(142, 6 + knobRow.implicitHeight + buttonRow.implicitHeight + sidechainRow.implicitHeight
                                      + 2 * 2 + 6)

    function rowY(row) {
        return contentTop + row * rowHeight
    }
    function showPage(page) {
        DeviceViews.setValue(editor.deviceId, "page", page)
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: {
            const fields = ["active", "in", "out", "above", "above_ratio", "below", "below_ratio", "attack", "release",
                            "solo"]
            const all = ["xover_low", "xover_high", "low_on", "high_on"]
            for (const band of ["low", "mid", "high"]) {
                for (const field of fields)
                    all.push(band + "_" + field)
            }
            return all.concat(["amount", "time", "output", "soft_knee", "mode", "sc_gain", "sc_mix", "sc_listen"])
        }
    }

    // The columns, as wide as their texts need in the fonts they are drawn in (so the editor fits whatever font the
    // UI gets): each box column its boxes' texts with the automation dot clear of them (EditorBoxWidth: the bands'
    // gains, ±24 dB; the thresholds, -80 to 0 dB; the ratios, 1:0.250 to 1:100; the times, 0.1 ms to 5 s; the
    // crossovers), and its caption whichever page is shown, as a Text lays it out (EditorTextsWidth).
    function bandParams(fields) {
        const all = []
        for (const band of ["low", "mid", "high"]) {
            for (const field of fields)
                all.push(p.get(band + "_" + field))
        }
        return all
    }
    EditorBoxWidth {
        id: levelBoxes
        params: editor.bandParams(["in", "out"])
    }
    EditorBoxWidth {
        id: fieldBoxes
        params: editor.bandParams(["above", "below", "attack"])
    }
    EditorBoxWidth {
        id: field2Boxes
        params: editor.bandParams(["above_ratio", "below_ratio", "release"])
    }
    EditorBoxWidth {
        id: crossoverBoxes
        params: [p.get("xover_low"), p.get("xover_high")]
    }
    EditorTextsWidth {
        id: levelCaptions
        texts: [qsTr("Input"), qsTr("Output")]
    }
    EditorTextsWidth {
        id: fieldCaptions
        texts: [qsTr("Above"), qsTr("Below"), qsTr("Attack")]
    }
    EditorTextsWidth {
        id: field2Captions
        texts: [qsTr("Ratio"), qsTr("Release")]
    }
    readonly property int levelBoxWidth: Math.max(levelBoxes.needed, levelCaptions.needed)
    readonly property int fieldWidth: Math.max(fieldBoxes.needed, fieldCaptions.needed)
    readonly property int field2Width: Math.max(field2Boxes.needed, field2Captions.needed)
    // The columns (x): the band column (its button and solo over the split's switch and crossover, as wide as
    // the wider needs, both filling it); Input; the display; the two fields; Output; the device's own controls.
    // Each follows the one before it.
    readonly property int bandWidth: Math.max(16 + crossoverBoxes.needed,
                                              Math.ceil(Math.max(highRow.buttonWidth, midRow.buttonWidth,
                                                                 lowRow.buttonWidth)) + 2 + 18)
    readonly property int inX: 8 + bandWidth + 6
    readonly property int graphX: inX + levelBoxWidth + 6
    readonly property int fieldX: graphX + graph.width + 6
    readonly property int field2X: fieldX + fieldWidth + 4
    readonly property int outX: field2X + field2Width + 6
    readonly property int globalX: outX + levelBoxWidth + 12

    // A box's formatter: its parameter's text for a value ("-20.0 dB", "1:4.00", "1:0.500", "10 ms"). Bound
    // to the parameter, so a box whose value never changes still gets its text once the parameter comes.
    function formatOf(param) {
        return param ? (v => param.format(v)) : null
    }
    // A frequency box's parser: "1.5k", "2 kHz", "800".
    function parserOf(param) {
        return param ? (t => param.parse(t)) : null
    }
    // A ratio box's: "1:4" (Live's way), "4", "4:1" (a compressor's), "1:inf".
    function parseRatio(text) {
        const r = graph.parseRatio(text)
        return r > 0 ? r : null
    }
    // A time box's, in ms: "250", "250 ms", "1.5 s".
    function parseTime(text) {
        return graph.parseTime(text)
    }

    // --- The header: the pages, and each column's name -----------------------------------------

    Row {
        objectName: "pages"
        x: 8
        y: 6
        spacing: 2

        Repeater {
            model: [
                { page: "T", name: "pageTime", tip: qsTr("Time: each band's attack and release") },
                { page: "B", name: "pageBelow", tip: qsTr("Below: each band's lower threshold and its ratio") },
                { page: "A", name: "pageAbove", tip: qsTr("Above: each band's upper threshold and its ratio") }
            ]
            RoleButton {
                required property var modelData
                objectName: modelData.name
                width: 18
                height: 16
                leftPadding: 0
                rightPadding: 0
                role: "small"
                text: modelData.page
                checkable: false
                checked: editor.fieldsPage === modelData.page
                tooltip: modelData.tip
                onClicked: editor.showPage(modelData.page)
            }
        }
    }
    EditorCaption {
        objectName: "inCaption"
        x: editor.inX
        y: 6
        width: editor.levelBoxWidth
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Input")
    }
    EditorCaption {
        objectName: "pageCaption"
        x: editor.fieldX
        y: 6
        width: editor.fieldWidth
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: editor.fieldsPage === "T" ? qsTr("Attack") : editor.fieldsPage === "B" ? qsTr("Below") : qsTr("Above")
    }
    EditorCaption {
        objectName: "pageCaption2"
        x: editor.field2X
        y: 6
        width: editor.field2Width
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: editor.fieldsPage === "T" ? qsTr("Release") : qsTr("Ratio")
    }
    EditorCaption {
        objectName: "outCaption"
        x: editor.outX
        y: 6
        width: editor.levelBoxWidth
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Output")
    }

    // --- The splits: High's under its row's buttons, Low's under its ---------------------------

    // A crossover and the switch beside it that splits its outer band off (not automatable, as Live's). Off,
    // the crossover dims: it does nothing until the band is split off again.
    component Split: Item {
        id: split

        property string band: "high"
        property int rowIndex: 0
        property string switchTip: ""
        property string boxTip: ""
        readonly property DeviceParam on: p.get(band + "_on")
        readonly property DeviceParam frequency: p.get(band === "high" ? "xover_high" : "xover_low")

        x: 8
        y: editor.rowY(rowIndex) + 19
        width: editor.bandWidth
        height: 18

        ParamButton {
            id: splitOn
            objectName: split.band + "On"
            width: 14
            height: 18
            role: "activator"
            param: split.on
            tooltip: split.switchTip
        }
        ParamBox {
            objectName: split.band === "high" ? "xoverHigh" : "xoverLow"
            x: 16
            width: split.width - 16  // (the band column: as wide as the crossover's texts need, or its buttons)
            param: split.frequency
            logScale: true
            decimals: 0
            defaultValue: split.frequency ? split.frequency.defaultValue : undefined
            formatter: editor.formatOf(split.frequency)
            parser: editor.parserOf(split.frequency)
            sampleText: crossoverBoxes.sample
            tooltip: split.boxTip
            opacity: splitOn.lit ? 1 : editor.dimmed
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
        }
    }
    Split {
        band: "high"
        rowIndex: 0
        switchTip: qsTr("Splits the high band off at this frequency. Off, its frequencies belong to the Mid band, "
                        + "which then shapes them with its own settings")
        boxTip: qsTr("Where the high band starts: the split between it and the mid band (24 dB/octave, "
                     + "phase-aligned)")
    }
    Split {
        band: "low"
        rowIndex: 2
        switchTip: qsTr("Splits the low band off at this frequency. Off, its frequencies belong to the Mid band, "
                        + "which then shapes them with its own settings")
        boxTip: qsTr("Where the low band ends: the split between it and the mid band (24 dB/octave, phase-aligned)")
    }

    // --- A band's row: its activator and solo, Input, the fields of the page shown, Output -----

    // A band's box, dimmed while the band is switched off or bypassed (still editable). A double-click resets it
    // to its parameter's default.
    component DimBox: ParamBox {
        property bool bandOn: true
        defaultValue: param ? param.defaultValue : undefined
        opacity: bandOn ? 1 : editor.dimmed
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    // The fields of one page, shown while it is chosen (fading in).
    component Page: Item {
        property string page: "A"
        visible: editor.fieldsPage === page
        opacity: visible ? 1 : 0
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }

    component BandRow: Item {
        id: row

        property string band: "mid"
        property string title: qsTr("Mid")
        property int rowIndex: 1
        // What its button's name needs (the band column is as wide as the widest band's).
        readonly property real buttonWidth: activator.implicitWidth
        // Split off (Mid always is); its activator on. Its controls dim unless both.
        readonly property bool on: band === "mid" || (p.get(band + "_on") ? p.get(band + "_on").value >= 0.5 : true)
        readonly property bool active: p.get(band + "_active") ? p.get(band + "_active").value >= 0.5 : true
        readonly property bool working: on && active
        readonly property real boxY: Math.round((height - 18) / 2)

        function param(field) {
            return p.get(band + "_" + field)
        }
        function name(field) {
            return band + field
        }

        x: 0
        y: editor.rowY(rowIndex)
        width: editor.width
        height: editor.rowHeight

        ParamButton {
            id: activator
            objectName: row.name("Active")
            x: 8
            y: 1
            width: editor.bandWidth - 20
            height: 16
            role: "activator"
            param: row.param("active")
            text: row.title
            opacity: row.on ? 1 : editor.dimmed
            tooltip: qsTr("The band's activator: off, its compression, expansion and gains are bypassed (its "
                          + "frequencies stay its own)")
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
        }
        ParamButton {
            objectName: row.name("Solo")
            x: 8 + editor.bandWidth - 18
            y: 1
            width: 18
            height: 16
            role: "solo"
            param: row.param("solo")
            text: "S"
            enabled: row.on  // (a band switched off has no sound of its own to solo)
            tooltip: qsTr("Solo: hear only the soloed bands")
        }
        DimBox {
            bandOn: row.working
            objectName: row.name("In")
            x: editor.inX
            y: row.boxY
            width: editor.levelBoxWidth
            param: row.param("in")
            step: 0.1
            decimals: 1
            formatter: editor.formatOf(row.param("in"))
            sampleText: levelBoxes.sample
            tooltip: qsTr("Gain before the band's dynamics (it moves the band's level against its thresholds)")
        }
        Page {
            page: "A"
            DimBox {
                bandOn: row.working
                objectName: row.name("Above")
                x: editor.fieldX
                y: row.boxY
                width: editor.fieldWidth
                param: row.param("above")
                step: 0.1
                decimals: 1
                formatter: editor.formatOf(row.param("above"))
                sampleText: fieldBoxes.sample
                tooltip: qsTr("Above: what happens to the band above this level. From 1:1 up it is compressed (at "
                              + "1:4, 4 dB over the threshold comes out as 1), under 1:1 (1:0.500) expanded upwards")
            }
            DimBox {
                bandOn: row.working
                objectName: row.name("AboveRatio")
                x: editor.field2X
                y: row.boxY
                width: editor.field2Width
                param: row.param("above_ratio")
                logScale: true
                decimals: 3
                formatter: editor.formatOf(row.param("above_ratio"))
                parser: t => editor.parseRatio(t)
                sampleText: field2Boxes.sample
                tooltip: qsTr("Above: what happens to the band above this level. From 1:1 up it is compressed (at "
                              + "1:4, 4 dB over the threshold comes out as 1), under 1:1 (1:0.500) expanded upwards")
            }
        }
        Page {
            page: "B"
            DimBox {
                bandOn: row.working
                objectName: row.name("Below")
                x: editor.fieldX
                y: row.boxY
                width: editor.fieldWidth
                param: row.param("below")
                step: 0.1
                decimals: 1
                formatter: editor.formatOf(row.param("below"))
                sampleText: fieldBoxes.sample
                tooltip: qsTr("Below: what happens to the band below this level. From 1:1 up it is pulled up "
                              + "(upward compression: at 1:4, 4 dB under the threshold comes out as 1), under 1:1 "
                              + "(1:0.500) pushed down (downward expansion)")
            }
            DimBox {
                bandOn: row.working
                objectName: row.name("BelowRatio")
                x: editor.field2X
                y: row.boxY
                width: editor.field2Width
                param: row.param("below_ratio")
                logScale: true
                decimals: 3
                formatter: editor.formatOf(row.param("below_ratio"))
                parser: t => editor.parseRatio(t)
                sampleText: field2Boxes.sample
                tooltip: qsTr("Below: what happens to the band below this level. From 1:1 up it is pulled up "
                              + "(upward compression: at 1:4, 4 dB under the threshold comes out as 1), under 1:1 "
                              + "(1:0.500) pushed down (downward expansion)")
            }
        }
        Page {
            page: "T"
            DimBox {
                bandOn: row.working
                objectName: row.name("Attack")
                x: editor.fieldX
                y: row.boxY
                width: editor.fieldWidth
                param: row.param("attack")
                logScale: true
                decimals: 2
                formatter: editor.formatOf(row.param("attack"))
                parser: t => editor.parseTime(t)
                sampleText: fieldBoxes.sample
                tooltip: qsTr("Attack: how fast the band's compression or expansion comes when its level crosses a "
                              + "threshold into a region")
            }
            DimBox {
                bandOn: row.working
                objectName: row.name("Release")
                x: editor.field2X
                y: row.boxY
                width: editor.field2Width
                param: row.param("release")
                logScale: true
                decimals: 2
                formatter: editor.formatOf(row.param("release"))
                parser: t => editor.parseTime(t)
                sampleText: field2Boxes.sample
                tooltip: qsTr("Release: how fast it lets go when the level comes back")
            }
        }
        DimBox {
            bandOn: row.working
            objectName: row.name("Out")
            x: editor.outX
            y: row.boxY
            width: editor.levelBoxWidth
            param: row.param("out")
            step: 0.1
            decimals: 1
            formatter: editor.formatOf(row.param("out"))
            sampleText: levelBoxes.sample
            tooltip: qsTr("Gain after the band's dynamics")
        }
    }

    BandRow {
        id: highRow
        band: "high"
        title: qsTr("High")
        rowIndex: 0
    }
    BandRow {
        id: midRow
        band: "mid"
        title: qsTr("Mid")
        rowIndex: 1
    }
    BandRow {
        id: lowRow
        band: "low"
        title: qsTr("Low")
        rowIndex: 2
    }

    // --- The display -------------------------------------------------------------------------

    MultibandGraph {
        id: graph
        objectName: "multibandGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: editor.graphX
        y: 6
        width: implicitWidth  // (MultibandGraph::kWidth)
        height: Math.max(implicitHeight, editor.height - 12)

        HoverHandler {
            id: graphHover
        }
        ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("Each band's level (thin bar: in, thick bar: out). Drag a block's edge for its threshold, "
                           + "inside it up or down for its ratio; Ctrl: every band; Alt: both thresholds; Shift: "
                           + "finely; double-click: reset")
    }

    // --- The device's own controls -----------------------------------------------------------

    // Their widths: a knob's cell as wide as its caption and every text its readout takes (EditorKnob's texts(),
    // as a Text lays them out: Amount 0 to 100 %, Time 10 to 1000 %, Output ±24 dB, the device's three at least
    // EditorKnob's own 52 px; S/C Gain -70 to 24 dB, S/C Mix 0 to 100 %), and the column as wide as its widest
    // row needs. The button rows fill it, so their edges line up (Soft Knee, Peak and RMS sharing it as their
    // texts need, Peak and RMS alike), and the knobs are centred in it.
    EditorTextsWidth {
        id: knobTexts
        knobs: [amountKnob, timeKnob, outputKnob]
    }
    EditorTextsWidth {
        id: scKnobTexts
        knobs: [scGain, scMix]
    }
    readonly property int knobCellWidth: Math.max(52, knobTexts.needed)
    readonly property int scKnobWidth: scKnobTexts.needed

    Column {
        id: globals
        objectName: "globals"
        x: editor.globalX
        y: 6
        width: Math.max(knobRow.implicitWidth, softKneeWidth + 2 * (modeImplicitWidth + 2),
                        2 * (editor.scKnobWidth + 2) + Math.ceil(Math.max(sidechainButton.implicitWidth,
                                                                          listen.implicitWidth)))
        // (what Soft Knee, Peak and RMS need, and Peak's and RMS's share of the row's room in proportion)
        readonly property int softKneeWidth: Math.ceil(softKnee.implicitWidth)
        readonly property int modeImplicitWidth: Math.ceil(Math.max(modePeak.implicitWidth, modeRms.implicitWidth))
        readonly property int modeWidth: Math.max(modeImplicitWidth,
                                                  Math.floor((width - 4) * modeImplicitWidth
                                                             / (softKneeWidth + 2 * modeImplicitWidth)))
        // Spread over the body's height (2 px apart at the least), as the band rows are.
        spacing: Math.max(2, Math.floor((editor.height - 12 - knobRow.height - buttonRow.height
                                         - sidechainRow.height) / 2))

        Row {
            id: knobRow
            objectName: "globalsKnobs"
            x: Math.floor((globals.width - width) / 2)
            spacing: 2

            EditorKnob {
                id: amountKnob
                objectName: "amount"
                width: editor.knobCellWidth
                size: 28
                param: p.get("amount")
                title: qsTr("Amount")
                tooltip: qsTr("How much of every ratio applies: at 0 % nothing is compressed or expanded")
            }
            EditorKnob {
                id: timeKnob
                objectName: "time"
                width: editor.knobCellWidth
                size: 28
                param: p.get("time")
                title: qsTr("Time")
                tooltip: qsTr("Scales every band's attack and release")
            }
            EditorKnob {
                id: outputKnob
                objectName: "output"
                width: editor.knobCellWidth
                size: 28
                knob.bipolar: true
                param: p.get("output")
                title: qsTr("Output")
                tooltip: qsTr("The device's output gain")
            }
        }
        Row {
            id: buttonRow
            objectName: "globalsButtons"
            spacing: 2

            ParamButton {
                id: softKnee
                objectName: "softKnee"
                width: globals.width - 2 * (globals.modeWidth + 2)
                height: 16
                param: p.get("soft_knee")
                text: qsTr("Soft Knee")
                tooltip: qsTr("Compression and expansion begin gradually around the thresholds (6 dB wide)")
            }
            ParamButton {
                id: modePeak
                objectName: "modePeak"
                width: globals.modeWidth
                height: 16
                param: p.get("mode")
                choice: 0
                text: qsTr("Peak")
                tooltip: qsTr("Peak reacts to short peaks; RMS to the average level, ignoring short peaks")
            }
            ParamButton {
                id: modeRms
                objectName: "modeRms"
                width: globals.modeWidth
                height: 16
                param: p.get("mode")
                choice: 1
                text: qsTr("RMS")
                tooltip: qsTr("Peak reacts to short peaks; RMS to the average level, ignoring short peaks")
            }
        }
        // The sidechain's: its Gain and Mix (dimmed until a sidechain is chosen, but settable first, as the
        // Gate's), the button that chooses one, and Listen.
        Row {
            id: sidechainRow
            objectName: "globalsSidechain"
            spacing: 2

            EditorKnob {
                id: scGain
                objectName: "scGain"
                width: editor.scKnobWidth
                size: 24
                param: p.get("sc_gain")
                title: qsTr("S/C Gain")
                opacity: graph.sidechained ? 1 : editor.dimmed
                tooltip: qsTr("The sidechain's level")
            }
            EditorKnob {
                id: scMix
                objectName: "scMix"
                width: editor.scKnobWidth
                size: 24
                param: p.get("sc_mix")
                title: qsTr("S/C Mix")
                opacity: graph.sidechained ? 1 : editor.dimmed
                tooltip: qsTr("How much of the trigger is the sidechain (100 %) rather than the device's own input "
                              + "(0 %)")
            }
            Item {
                width: globals.width - 2 * (editor.scKnobWidth + 2)
                height: scGain.height

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    spacing: 4

                    RoleButton {
                        id: sidechainButton
                        objectName: "sidechainButton"
                        width: parent.width
                        height: 16
                        role: "small"
                        text: qsTr("Sidechain")
                        checkable: false
                        checked: graph.sidechained
                        tooltip: qsTr("Choose what keys the bands: each band hears its own band of it")
                        onClicked: editor.sidechainMenuRequested(sidechainButton)
                    }
                    ParamButton {
                        id: listen
                        objectName: "scListen"
                        width: parent.width
                        height: 16
                        param: p.get("sc_listen")
                        iconName: "headphones"
                        iconSize: 11
                        text: qsTr("Listen")
                        tooltip: qsTr("Listen: hear what the bands' detectors hear (the sidechain, as much of it as "
                                      + "S/C Mix takes) instead of the output")
                    }
                }
            }
        }
    }
}
