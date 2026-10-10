import QtQuick
import QtQuick.Controls
import SUBstation

// Multiband Dynamics' editor, laid out as Ableton's: a row per band, High on
// top (its switch and solo, the crossover under High's and Low's switch; the
// band's Input; its lane of the display; two fields the T/B/A buttons switch
// between its Time (attack, release), Below and Above (threshold, ratio); its
// Output), and at the right the device's Amount, Time and Output, Soft Knee,
// Peak/RMS and the sidechain's Gain, Mix and button. The display
// (MultibandGraph) shows each band's level before and after its dynamics and
// its two regions: drag a region's edge for its threshold, inside it up or
// down for its ratio. A band switched off dims its controls (they stay
// editable, as Live keeps them: the Mid band then shapes its frequencies).
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click
// gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    // Which fields the bands show: "T" (attack, release), "B" (Below) or "A" (Above). View state.
    property string fieldsPage: "A"
    // The rows, as the graph's lanes: High, Mid, Low under a 16 px header.
    readonly property real rowHeight: graph.rowHeight
    readonly property int contentTop: 22

    signal sidechainMenuRequested()

    implicitWidth: 780
    // At the least: the header and three rows of 38 px, or the device's controls 2 px apart.
    implicitHeight: Math.max(142, 6 + knobRow.implicitHeight + buttonRow.implicitHeight + sidechainRow.implicitHeight
                                      + 2 * 2 + 6)

    function rowY(row) {
        return contentTop + row * rowHeight
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: {
            const fields = ["in", "out", "above", "above_ratio", "below", "below_ratio", "attack", "release", "solo"]
            const all = ["xover_low", "xover_high", "low_on", "high_on"]
            for (const band of ["low", "mid", "high"]) {
                for (const field of fields)
                    all.push(band + "_" + field)
            }
            return all.concat(["amount", "time", "output", "soft_knee", "mode", "sc_gain", "sc_mix"])
        }
    }

    // A box's formatter: its parameter's text for a value ("-20.0 dB", "4.00:1", "1:2.00", "10 ms"). Bound
    // to the parameter, so a box whose value never changes still gets its text once the parameter comes.
    function formatOf(param) {
        return param ? (v => param.format(v)) : null
    }
    // A frequency box's parser: "1.5k", "2 kHz", "800".
    function parserOf(param) {
        return param ? (t => param.parse(t)) : null
    }

    // --- The header ---------------------------------------------------------------------------

    EditorCaption {
        x: 80
        y: 6
        width: 50
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Input")
    }
    Row {
        x: 442
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
                onClicked: editor.fieldsPage = modelData.page
            }
        }
    }
    EditorCaption {
        objectName: "pageCaption"
        x: 502
        y: 6
        width: 42
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: editor.fieldsPage === "T" ? qsTr("Time") : editor.fieldsPage === "B" ? qsTr("Below") : qsTr("Above")
    }
    EditorCaption {
        x: 550
        y: 6
        width: 50
        height: 16
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Output")
    }

    // --- The band column: the switches and crossovers (the solos are the rows') ----------------

    ParamButton {
        id: highOn
        objectName: "highOn"
        x: 8
        y: editor.rowY(0) + 1
        width: 44
        height: 16
        role: "activator"
        param: p.get("high_on")
        text: qsTr("High")
        tooltip: qsTr("Switches the high band on. Off, its frequencies belong to the Mid band, which then shapes them with its own settings")
    }
    ParamBox {
        objectName: "xoverHigh"
        x: 8
        y: editor.rowY(0) + 19
        width: 66
        param: p.get("xover_high")
        logScale: true
        decimals: 0
        defaultValue: 2500
        formatter: editor.formatOf(p.get("xover_high"))
        parser: editor.parserOf(p.get("xover_high"))
        sampleText: "18.00 kHz"
        tooltip: qsTr("Where the high band starts: the split between it and the mid band (24 dB/octave, phase-aligned)")
        opacity: highOn.lit ? 1 : 0.45
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    Rectangle {
        objectName: "midLabel"
        x: 8
        y: editor.rowY(1) + 1
        width: 44
        height: 16
        radius: 2
        color: Theme.activatorOn
        border.width: 1
        border.color: Theme.border

        Text {
            anchors.centerIn: parent
            text: qsTr("Mid")
            color: Theme.accentText
            font.family: Theme.uiFont(8).family
            font.pointSize: 8
            font.weight: Font.DemiBold
        }
        HoverHandler {
            id: midHover
        }
        ToolTip.visible: midHover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("The mid band is always on: with High and Low off it covers the whole spectrum")
    }
    ParamButton {
        id: lowOn
        objectName: "lowOn"
        x: 8
        y: editor.rowY(2) + 1
        width: 44
        height: 16
        role: "activator"
        param: p.get("low_on")
        text: qsTr("Low")
        tooltip: qsTr("Switches the low band on. Off, its frequencies belong to the Mid band, which then shapes them with its own settings")
    }
    ParamBox {
        objectName: "xoverLow"
        x: 8
        y: editor.rowY(2) + 19
        width: 66
        param: p.get("xover_low")
        logScale: true
        decimals: 0
        defaultValue: 120
        formatter: editor.formatOf(p.get("xover_low"))
        parser: editor.parserOf(p.get("xover_low"))
        sampleText: "18.00 kHz"
        tooltip: qsTr("Where the low band ends: the split between it and the mid band (24 dB/octave, phase-aligned)")
        opacity: lowOn.lit ? 1 : 0.45
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }

    // --- A band's row: solo, Input, the fields of the page shown, Output -----------------------

    // A band's box, dimmed while the band is switched off (still editable).
    component DimBox: ParamBox {
        property bool bandOn: true
        opacity: bandOn ? 1 : 0.45
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
        property int rowIndex: 1
        // Mid is always on; High and Low follow their switch (their controls dim while off).
        readonly property bool on: band === "mid" || (p.get(band + "_on") ? p.get(band + "_on").value >= 0.5 : true)
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
            objectName: row.name("Solo")
            x: 56
            y: 1
            width: 18
            height: 16
            role: "solo"
            param: row.param("solo")
            text: "S"
            enabled: row.on
            tooltip: qsTr("Solo: hear only the soloed bands")
        }
        DimBox {
            bandOn: row.on
            objectName: row.name("In")
            x: 80
            y: row.boxY
            width: 50
            param: row.param("in")
            step: 0.1
            decimals: 1
            defaultValue: 0
            formatter: editor.formatOf(row.param("in"))
            sampleText: "-24.0 dB"
            tooltip: qsTr("Gain before the band's dynamics (it moves the band's level against its thresholds)")
        }
        Page {
            page: "A"
            DimBox {
                bandOn: row.on
                objectName: row.name("Above")
                x: 442
                y: row.boxY
                width: 56
                param: row.param("above")
                step: 0.1
                decimals: 1
                defaultValue: -20
                formatter: editor.formatOf(row.param("above"))
                sampleText: "-80.0 dB"
                tooltip: qsTr("Above: what happens to the band above this level. Over 1:1 it is compressed (down), under 1:1 expanded (up)")
            }
            DimBox {
                bandOn: row.on
                objectName: row.name("AboveRatio")
                x: 502
                y: row.boxY
                width: 42
                param: row.param("above_ratio")
                logScale: true
                decimals: 3
                defaultValue: 1
                formatter: editor.formatOf(row.param("above_ratio"))
                parser: t => {
                    const r = graph.parseRatio(t)
                    return r > 0 ? r : null
                }
                sampleText: "1:4.00"
                tooltip: qsTr("Above: what happens to the band above this level. Over 1:1 it is compressed (down), under 1:1 expanded (up)")
            }
        }
        Page {
            page: "B"
            DimBox {
                bandOn: row.on
                objectName: row.name("Below")
                x: 442
                y: row.boxY
                width: 56
                param: row.param("below")
                step: 0.1
                decimals: 1
                defaultValue: -40
                formatter: editor.formatOf(row.param("below"))
                sampleText: "-80.0 dB"
                tooltip: qsTr("Below: what happens to the band below this level. Over 1:1 it is pulled up (upward compression), under 1:1 pushed down (downward expansion)")
            }
            DimBox {
                bandOn: row.on
                objectName: row.name("BelowRatio")
                x: 502
                y: row.boxY
                width: 42
                param: row.param("below_ratio")
                logScale: true
                decimals: 3
                defaultValue: 1
                formatter: editor.formatOf(row.param("below_ratio"))
                parser: t => {
                    const r = graph.parseRatio(t)
                    return r > 0 ? r : null
                }
                sampleText: "1:4.00"
                tooltip: qsTr("Below: what happens to the band below this level. Over 1:1 it is pulled up (upward compression), under 1:1 pushed down (downward expansion)")
            }
        }
        Page {
            page: "T"
            DimBox {
                bandOn: row.on
                objectName: row.name("Attack")
                x: 442
                y: row.boxY
                width: 56
                param: row.param("attack")
                logScale: true
                decimals: 2
                defaultValue: 10
                formatter: editor.formatOf(row.param("attack"))
                sampleText: "999 ms"
                tooltip: qsTr("Attack: how fast the band's compression or expansion comes when its level crosses a threshold into a region")
            }
            DimBox {
                bandOn: row.on
                objectName: row.name("Release")
                x: 502
                y: row.boxY
                width: 42
                param: row.param("release")
                logScale: true
                decimals: 1
                defaultValue: 100
                formatter: editor.formatOf(row.param("release"))
                sampleText: "999 ms"
                tooltip: qsTr("Release: how fast it lets go when the level comes back")
            }
        }
        DimBox {
            bandOn: row.on
            objectName: row.name("Out")
            x: 550
            y: row.boxY
            width: 50
            param: row.param("out")
            step: 0.1
            decimals: 1
            defaultValue: 0
            formatter: editor.formatOf(row.param("out"))
            sampleText: "-24.0 dB"
            tooltip: qsTr("Gain after the band's dynamics")
        }
    }

    BandRow {
        band: "high"
        rowIndex: 0
    }
    BandRow {
        band: "mid"
        rowIndex: 1
    }
    BandRow {
        band: "low"
        rowIndex: 2
    }

    // --- The display -------------------------------------------------------------------------

    MultibandGraph {
        id: graph
        objectName: "multibandGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 136
        y: 6
        width: 300
        height: Math.max(implicitHeight, editor.height - 12)

        HoverHandler {
            id: graphHover
        }
        ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("Each band's level (thin bar: in, thick bar: out). Drag a block's edge for its threshold, inside it up or down for its ratio; Ctrl: every band; Alt: both thresholds; Shift: finely; double-click: reset")
    }

    // --- The device's own controls -----------------------------------------------------------

    Column {
        id: globals
        objectName: "globals"
        x: 612
        y: 6
        width: 160
        // Spread over the body's height (2 px apart at the least), as the band rows are.
        spacing: Math.max(2, Math.floor((editor.height - 12 - knobRow.height - buttonRow.height
                                         - sidechainRow.height) / 2))

        Row {
            id: knobRow
            objectName: "globalsKnobs"
            spacing: 2

            EditorKnob {
                objectName: "amount"
                size: 28
                param: p.get("amount")
                title: qsTr("Amount")
                tooltip: qsTr("How much of every ratio applies: at 0 % nothing is compressed or expanded")
            }
            EditorKnob {
                objectName: "time"
                size: 28
                param: p.get("time")
                title: qsTr("Time")
                tooltip: qsTr("Scales every band's attack and release")
            }
            EditorKnob {
                objectName: "output"
                size: 28
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
                objectName: "softKnee"
                width: 72
                height: 16
                param: p.get("soft_knee")
                text: qsTr("Soft Knee")
                tooltip: qsTr("Compression and expansion begin gradually around the thresholds (6 dB wide)")
            }
            ParamButton {
                objectName: "modePeak"
                width: 42
                height: 16
                param: p.get("mode")
                choice: 0
                text: qsTr("Peak")
                tooltip: qsTr("Peak reacts to short peaks; RMS to the average level, ignoring short peaks")
            }
            ParamButton {
                objectName: "modeRms"
                width: 42
                height: 16
                param: p.get("mode")
                choice: 1
                text: qsTr("RMS")
                tooltip: qsTr("Peak reacts to short peaks; RMS to the average level, ignoring short peaks")
            }
        }
        Row {
            id: sidechainRow
            objectName: "globalsSidechain"
            spacing: 2

            EditorKnob {
                id: scGain
                objectName: "scGain"
                size: 22
                param: p.get("sc_gain")
                title: qsTr("SC Gain")
                enabled: graph.sidechained
                tooltip: qsTr("The sidechain's level")
            }
            EditorKnob {
                objectName: "scMix"
                size: 22
                param: p.get("sc_mix")
                title: qsTr("SC Mix")
                enabled: graph.sidechained
                tooltip: qsTr("How much of the trigger is the sidechain (100 %) rather than the device's own input (0 %)")
            }
            Item {
                width: 52
                height: scGain.height

                RoleButton {
                    objectName: "sidechainButton"
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 16
                    leftPadding: 2
                    rightPadding: 2
                    role: "small"
                    text: qsTr("Sidechain")
                    checkable: false
                    checked: graph.sidechained
                    tooltip: qsTr("Choose what keys the bands: each band hears its own band of it")
                    onClicked: editor.sidechainMenuRequested()
                }
            }
        }
    }
}
