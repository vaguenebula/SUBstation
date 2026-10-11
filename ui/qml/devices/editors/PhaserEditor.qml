import QtQuick
import QtQuick.Controls
import SUBstation

// The Phaser-Flanger's editor, laid out as Ableton's: the mode tabs (Phaser,
// Flanger, Doubler) at the left; the mode's controls (Notches, Center, Spread,
// Blend, or the delay's Time); the graph (PhaserGraph: the response that plays,
// moving with the modulation, over the LFO's shape and its phase); the LFO
// (Freq or a synced Rate, the waveform, Duty Cycle, Phase or Spin); the globals
// (Amount, Feedback and its Ø, Warmth, Output, Dry/Wet); and, shown by More,
// LFO 2, the envelope follower and Safe Bass (whether it is open is view
// state, by device: DeviceViews). Controls that only swap with a switch
// (Freq/Rate, Phase/Spin, the delay's Time) rebind in place. Every control
// shows its parameter as it is now (its automation's value while that plays),
// sets it undoably, touches it when pressed, and right-click gives its menu.
// Everything with text is as wide as the font makes its text (the knobs'
// cells their widest caption or readout, the tabs their names), and the
// editor as wide as its sections: 732 px in the default font, 906 with More.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property int margin: 8
    // A knob's cell: as wide as its widest caption or readout (the house's 52 px, an EditorKnob's width at the
    // house size, at least), a synced rate's caption centred with room for its ♪ at the cell's right, a pixel
    // clear, and the buttons a cell holds (Spin, Ø, More, Env) their text with 2 px either side (the border and a
    // pixel clear); on even pixels, so that the 34 px knob centres under its caption and readout exactly.
    readonly property int cell: even(Math.max(52, cellTexts.implicitWidth,
                                              syncedTexts.implicitWidth + 2 * (syncSize + 1),
                                              Math.max(spinButton.button.implicitContentWidth,
                                                       invertButton.button.implicitContentWidth,
                                                       expandButton.implicitContentWidth,
                                                       envAmount.switchContentWidth) + 4))
    readonly property int knobSize: 34
    readonly property int syncSize: 12  // the ♪ switch
    readonly property int gap: 4        // between cells
    readonly property int sectionGap: 10
    // The mode tabs: 58 px, or the widest tab's own width.
    readonly property int tabWidth: Math.max(58, Math.ceil(Array.from(tabs.children).reduce(
        (widest, tab) => Math.max(widest, tab.implicitWidth), 0)))
    // The delay's Time knob's cell: as wide as its caption or readout (an EditorKnob's width at its size, 60 px,
    // at least), on even pixels as the others.
    readonly property int timeWidth: even(Math.max(time.size + 16, timeTexts.implicitWidth))
    // The mode's section: two cells, or the delay's Time and the Flanger's notch under it (centred in it, so on
    // even pixels too); the LFO's: two cells.
    readonly property int modeWidth: even(Math.max(2 * cell + gap, timeWidth, notchTexts.implicitWidth))
    readonly property int pairWidth: 2 * cell + gap
    readonly property int tripleWidth: 3 * cell + 2 * gap  // the globals and the extra section
    readonly property int graphWidth: graph.implicitWidth  // (PhaserGraph's own: 240 px)

    // Where each section starts.
    readonly property int modeX: margin + tabWidth + 8
    readonly property int graphX: modeX + modeWidth + sectionGap
    readonly property int lfoX: graphX + graphWidth + sectionGap
    readonly property int globalsX: lfoX + pairWidth + sectionGap
    readonly property int extraX: globalsX + tripleWidth + sectionGap

    // Two rows of knobs, centred in the height there is.
    readonly property real rowHeight: notches.implicitHeight
    readonly property real rowsHeight: 2 * rowHeight + gap
    readonly property real rowsY: Math.round((height - rowsHeight) / 2)

    readonly property int mode: p.get("mode") ? p.get("mode").index : 0  // 0 Phaser, 1 Flanger, 2 Doubler
    readonly property bool synced: on("lfo_sync")
    readonly property bool spinning: on("spin_on")
    readonly property bool synced2: on("lfo2_sync")
    readonly property bool envOn: on("env_on")
    // More: LFO 2, the envelope follower and Safe Bass shown (not saved: how the device is looked at).
    readonly property bool expanded: DeviceViews.value(deviceId, "expanded", false)

    // The body: the sections and the gaps between them, and the extra section while shown.
    implicitWidth: globalsX + tripleWidth + (expanded ? sectionGap + tripleWidth : 0) + margin
    implicitHeight: 6 + Math.max(rowsHeight, time.implicitHeight + delayKnobs.spacing + notchReadout.implicitHeight,
                                 graph.implicitHeight, 3 * 16 + 8) + 6

    // `width` rounded up to even pixels: something 34 or 44 px wide centres in it on whole ones.
    function even(width) {
        return 2 * Math.ceil(width / 2)
    }

    function on(id) {
        const param = p.get(id)
        return param ? param.value >= 0.5 : false
    }

    function setExpanded(open) {
        DeviceViews.setValue(deviceId, "expanded", open)
    }

    // The widest of `lines` as a Text lays them out in its font (as a caption's, a readout's or a button's text
    // is as wide as its advance and whatever of its last glyph reaches past it): hidden, a line each.
    component Widest: Text {
        property var lines: []
        visible: false
        textFormat: Text.PlainText
        text: lines.join("\n")
        font: Theme.uiFont(8)  // (EditorCaption's and EditorReadout's)
    }
    // What the knobs' cells show at their widest: every caption (the swapped knobs' both), and each readout's
    // widest form, every figure the font's widest (figures may be proportional), so no value is cut short,
    // whatever the font: Center's "1#.## kHz" (and 999.6 Hz reads "1000 Hz", 9999.6 Hz "10.00 kHz"); the
    // LFOs' and Safe Bass's "##.## Hz" (9.996 Hz reads "10.00 Hz"); the synced rates' divisions; the
    // percentages, signed for Duty and Env; Blend's "#.##"; Phase's degrees; Output's "-##.# dB"; the envelope's
    // times ("#.## ms", "##.# ms", "### ms").
    Widest {
        id: cellTexts
        lines: [notches, centerKnob, spreadKnob, blendKnob, duty, amountKnob, feedbackKnob, warmthKnob, outputKnob,
                mixKnob, lfo2MixKnob, safeBassKnob, attackKnob, releaseKnob].map(knob => knob.title)
            .concat([qsTr("Freq"), qsTr("Rate"), qsTr("Phase"), qsTr("Spin")])
            .concat(["#### Hz", "#.## kHz", "1#.## kHz", "##.## Hz", "### %", "-### %", "#.##", "###°", "-##.# dB",
                     "#.## ms", "##.# ms", "### ms"].map(pattern => figures.sample(pattern)))
            .concat(p.get("lfo_rate") ? p.get("lfo_rate").labels : [])
    }
    // The synced rates' captions, beside their ♪.
    Widest {
        id: syncedTexts
        lines: [qsTr("Freq"), qsTr("Rate")]
    }
    // The delay's Time (its caption and readouts: the Flanger's "#.## ms" to "##.# ms", the Doubler's
    // "### ms"), and the Flanger's first notch under it ("Notch 5.00 kHz" at 0.1 ms).
    Widest {
        id: timeTexts
        lines: [time.title].concat(["#.## ms", "##.# ms", "### ms"].map(pattern => figures.sample(pattern)))
    }
    Widest {
        id: notchTexts
        lines: ["#### Hz", "#.## kHz"].map(pattern => "Notch " + figures.sample(pattern))  // (the graph's notchText)
    }
    // The readouts' figures.
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

        font: Theme.uiFont(8)
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["mode", "notches", "center", "spread", "blend", "flange_time", "doubler_time", "amount", "feedback",
              "fb_invert", "lfo_sync", "lfo_freq", "lfo_rate", "lfo_wave", "lfo_duty", "spin_on", "phase", "spin",
              "lfo2_mix", "lfo2_sync", "lfo2_freq", "lfo2_rate", "env_on", "env_amount", "env_attack", "env_release",
              "safe_bass", "warmth", "output", "mix"]
    }

    // A knob in its cell (the editor's), its caption over it and its readout under it.
    component CellKnob: EditorKnob {
        width: editor.cell
        size: editor.knobSize
    }

    // A knob whose rate can be synced: the ♪ switch at the right of its caption. Synced, it steps
    // through note values, and the wheel moves it one a notch (the knob's own wheel moves a fiftieth
    // of the range, which a list of 22 rounds back to where it was).
    component SyncedKnob: Item {
        id: synced

        property alias knob: knob
        property alias syncButton: syncButton
        property string knobName
        property string buttonName
        readonly property bool stepping: knob.step > 0

        width: knob.width
        height: knob.implicitHeight

        EditorKnob {
            id: knob
            objectName: synced.knobName
            width: editor.cell
            size: editor.knobSize
            knob.knob.wheel: !synced.stepping
        }
        WheelHandler {
            property real pending: 0  // (a fine-grained wheel's eighths of a notch, until they make one)

            enabled: synced.stepping
            onWheel: event => {
                pending += event.angleDelta.y
                const notches = pending > 0 ? Math.floor(pending / 120) : Math.ceil(pending / 120)
                pending -= notches * 120
                const param = knob.param
                if (notches === 0 || !param)
                    return
                const next = Math.max(param.minimum, Math.min(param.maximum, Math.round(param.value) + notches))
                if (next !== param.value)
                    param.set(next, "")
            }
        }
        // At the cell's right in the caption's row (no taller than it, so it stays clear of the dial).
        ParamButton {
            id: syncButton
            objectName: synced.buttonName
            x: synced.width - width
            y: 0
            width: editor.syncSize
            height: Math.min(editor.syncSize, knob.knob.y)
            iconName: "note"
            iconSize: 9
            tooltip: qsTr("Sync the LFO to the song's tempo")
            button.leftPadding: 0
            button.rightPadding: 0
            button.topPadding: 0
            button.bottomPadding: 0
        }
    }

    // Env Follow's cell: its switch where the caption would be, the amount's knob and value under it.
    component EnvCell: Column {
        id: envCell

        property alias knob: envKnob
        readonly property real switchContentWidth: envSwitch.button.implicitContentWidth
        property bool dimmed: false

        width: editor.cell
        spacing: 1

        ParamButton {
            id: envSwitch
            objectName: "envOn"
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(envCell.width, Math.max(40, implicitWidth))
            height: notches.knob.y - 1
            param: p.get("env_on")
            text: qsTr("Env")
            tooltip: qsTr("Follow the input's level")
        }
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: editor.knobSize
            height: editor.knobSize
            opacity: envCell.dimmed ? 0.55 : 1
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            ParamKnob {
                id: envKnob
                anchors.fill: parent
                size: editor.knobSize
                bipolar: true
                param: p.get("env_amount")

                HoverHandler {
                    id: envHover
                }
                ToolTip.visible: envHover.hovered && !envKnob.knob.dragging
                ToolTip.delay: 700
                ToolTip.text: qsTr("How far the level moves the sweep (negative: the other way)")
            }
        }
        EditorReadout {
            width: parent.width
            text: envKnob.param ? envKnob.param.text : ""
            elide: Text.ElideRight
            opacity: envCell.dimmed ? 0.55 : 1
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
        }
    }

    // --- The mode tabs ---------------------------------------------------------------------

    Column {
        id: tabs
        x: editor.margin
        y: 6
        width: editor.tabWidth
        spacing: 4

        Repeater {
            model: [[qsTr("Phaser"), "modePhaser",
                     qsTr("Phaser: notches from a chain of all-pass filters, swept by the LFO")],
                    [qsTr("Flanger"), "modeFlanger", qsTr("Flanger: a comb from a short delay, swept by the LFO")],
                    [qsTr("Doubler"), "modeDoubler", qsTr("Doubler: a longer, gently moving delay: a second take")]]
            ParamButton {
                required property var modelData
                required property int index
                // Whole pixels (45, 44, 45 in the body's 154): no edge falls between two rows.
                readonly property real share: (editor.height - 12 - 2 * tabs.spacing) / 3
                objectName: modelData[1]
                width: tabs.width
                height: Math.round((index + 1) * share) - Math.round(index * share)
                param: p.get("mode")
                choice: index
                text: modelData[0]
                tooltip: modelData[2]
            }
        }
    }

    EditorDivider {
        x: editor.margin + editor.tabWidth + 3
    }

    // --- The mode's controls ---------------------------------------------------------------

    Item {
        id: modeSection
        x: editor.modeX
        y: 0
        width: editor.modeWidth
        height: editor.height

        // The Phaser's: its notches, where they are, how far apart, and what the modulation moves.
        Grid {
            id: phaserKnobs
            objectName: "phaserKnobs"
            x: (parent.width - width) / 2  // (the section may be wider, for the delay's texts)
            y: editor.rowsY
            columns: 2
            columnSpacing: editor.gap
            rowSpacing: editor.gap
            opacity: editor.mode === 0 ? 1 : 0
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            CellKnob {
                id: notches
                objectName: "notches"
                step: 1
                param: p.get("notches")
                title: qsTr("Notches")
                tooltip: qsTr("How many all-pass filters, one notch each")
            }
            CellKnob {
                id: centerKnob
                objectName: "center"
                param: p.get("center")
                title: qsTr("Center")
                tooltip: qsTr("Where the notches sit")
            }
            CellKnob {
                id: spreadKnob
                objectName: "spread"
                param: p.get("spread")
                title: qsTr("Spread")
                tooltip: qsTr("How far apart the notches are (the filters' Q)")
            }
            CellKnob {
                id: blendKnob
                objectName: "blend"
                param: p.get("blend")
                title: qsTr("Blend")
                formatter: v => v.toFixed(2)
                tooltip: qsTr("What the modulation moves: Center at 0, Spread at 1")
            }
        }

        // The Flanger's and the Doubler's: the delay (and the Flanger's first notch).
        Column {
            id: delayKnobs
            objectName: "delayKnobs"
            width: parent.width
            // Centred with the Flanger's notch under the knob, in the Doubler too: the knob stays put.
            y: Math.round((editor.height - time.implicitHeight - spacing - notchReadout.implicitHeight) / 2)
            spacing: 4
            opacity: editor.mode !== 0 ? 1 : 0
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            EditorKnob {
                id: time
                objectName: "time"
                anchors.horizontalCenter: parent.horizontalCenter
                width: editor.timeWidth
                size: 44
                param: editor.mode === 2 ? p.get("doubler_time") : p.get("flange_time")
                title: qsTr("Time")
                tooltip: qsTr("The delay: shorter moves the comb's notches up")
            }
            EditorReadout {
                id: notchReadout
                objectName: "notchReadout"
                width: parent.width
                visible: editor.mode === 1
                color: Theme.textDim
                text: graph.notchText
            }
        }
    }

    EditorDivider {
        x: editor.graphX - 5
    }

    // --- The graph -------------------------------------------------------------------------

    PhaserGraph {
        id: graph
        objectName: "phaserGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: editor.graphX
        y: 6
        width: editor.graphWidth
        height: Math.max(implicitHeight, editor.height - 12)

        HoverHandler {
            id: graphHover
        }
        ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("The response as it plays now. Drag across for the Center (Flanger: the first notch; "
                           + "Doubler: the Time), up and down for the Spread (the Feedback); double-click to reset")
    }

    EditorDivider {
        x: editor.lfoX - 5
    }

    // --- The LFO ---------------------------------------------------------------------------

    Grid {
        id: lfoSection
        x: editor.lfoX
        y: editor.rowsY
        columns: 2
        columnSpacing: editor.gap
        rowSpacing: editor.gap

        SyncedKnob {
            id: rate
            knobName: "rate"
            buttonName: "sync"
            knob.param: editor.synced ? p.get("lfo_rate") : p.get("lfo_freq")
            knob.step: editor.synced ? 1 : 0
            knob.title: editor.synced ? qsTr("Rate") : qsTr("Freq")
            knob.tooltip: qsTr("How fast the LFO moves (♪: in note values)")
            syncButton.param: p.get("lfo_sync")
        }
        Item {
            width: editor.cell
            height: editor.rowHeight

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                ParamChoice {
                    objectName: "wave"
                    width: editor.cell
                    param: p.get("lfo_wave")
                    iconOnly: true
                    icons: ["wave_sine", "wave_triangle", "wave_triangle", "wave_triangle", "wave_triangle",
                            "wave_saw_up", "wave_saw_down", "wave_square", "wave_random", "wave_random"]
                    tooltip: qsTr("The LFO's shape")
                }
                ParamButton {
                    id: spinButton
                    objectName: "spinOn"
                    width: editor.cell
                    param: p.get("spin_on")
                    text: qsTr("Spin")
                    tooltip: qsTr("Spin instead of a fixed phase")
                }
            }
        }
        CellKnob {
            id: duty
            objectName: "duty"
            param: p.get("lfo_duty")
            knob.bipolar: true
            title: qsTr("Duty")
            tooltip: qsTr("Bends the shape: a rectangle's width, the others' skew")
        }
        CellKnob {
            objectName: "phaseSpin"
            param: editor.spinning ? p.get("spin") : p.get("phase")
            title: editor.spinning ? qsTr("Spin") : qsTr("Phase")
            tooltip: editor.spinning ? qsTr("Spin: the right LFO runs faster than the left by this much")
                                     : qsTr("How far the right LFO runs from the left")
        }
    }

    EditorDivider {
        x: editor.globalsX - 5
    }

    // --- The globals -----------------------------------------------------------------------

    Grid {
        id: globals
        x: editor.globalsX
        y: editor.rowsY
        columns: 3
        columnSpacing: editor.gap
        rowSpacing: editor.gap

        CellKnob {
            id: amountKnob
            objectName: "amount"
            param: p.get("amount")
            title: qsTr("Amount")
            tooltip: qsTr("How far the LFO and the envelope move the sweep")
        }
        CellKnob {
            id: feedbackKnob
            objectName: "feedback"
            param: p.get("feedback")
            title: qsTr("Feedback")
            tooltip: qsTr("The output fed back in: sharper, ringing notches")
        }
        Item {
            width: editor.cell
            height: editor.rowHeight

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                ParamButton {
                    id: invertButton
                    objectName: "fbInvert"
                    width: editor.cell
                    param: p.get("fb_invert")
                    text: "Ø"
                    tooltip: qsTr("Invert the feedback")
                }
                RoleButton {
                    id: expandButton
                    objectName: "expandButton"
                    width: editor.cell
                    height: 16
                    role: "small"
                    iconName: "sliders"
                    iconSize: 10
                    text: qsTr("More")
                    checkable: false
                    checked: editor.expanded
                    tooltip: editor.expanded ? qsTr("Hide LFO 2, the envelope follower and Safe Bass")
                                             : qsTr("Show LFO 2, the envelope follower and Safe Bass")
                    onClicked: editor.setExpanded(!editor.expanded)
                }
            }
        }
        CellKnob {
            id: warmthKnob
            objectName: "warmth"
            param: p.get("warmth")
            title: qsTr("Warmth")
            tooltip: qsTr("A little saturation and darkening of the effect")
        }
        CellKnob {
            id: outputKnob
            objectName: "output"
            param: p.get("output")
            title: qsTr("Output")
            tooltip: qsTr("Output level")
        }
        CellKnob {
            id: mixKnob
            objectName: "mix"
            param: p.get("mix")
            title: qsTr("Dry/Wet")
            tooltip: qsTr("50 %: the deepest notches; 100 %: only the moved signal")
        }
    }

    // --- More: LFO 2, the envelope follower, Safe Bass --------------------------------------

    Item {
        id: extraSection
        objectName: "extraSection"
        x: editor.extraX - 10
        width: editor.sectionGap + editor.tripleWidth
        height: editor.height
        visible: editor.expanded
        opacity: editor.expanded ? 1 : 0
        Behavior on opacity {
            NumberAnimation {
                duration: 150
            }
        }

        EditorDivider {
            x: 5
        }
        Grid {
            x: editor.sectionGap
            y: editor.rowsY
            columns: 3
            columnSpacing: editor.gap
            rowSpacing: editor.gap

            CellKnob {
                id: lfo2MixKnob
                objectName: "lfo2Mix"
                param: p.get("lfo2_mix")
                title: qsTr("LFO 2")
                tooltip: qsTr("Crossfade the modulation from LFO 1 to LFO 2 (a triangle)")
            }
            SyncedKnob {
                id: rate2
                knobName: "rate2"
                buttonName: "sync2"
                opacity: p.get("lfo2_mix") && p.get("lfo2_mix").value > 0 ? 1 : 0.55
                Behavior on opacity {
                    NumberAnimation {
                        duration: 120
                    }
                }
                knob.param: editor.synced2 ? p.get("lfo2_rate") : p.get("lfo2_freq")
                knob.step: editor.synced2 ? 1 : 0
                knob.title: editor.synced2 ? qsTr("Rate") : qsTr("Freq")  // (LFO 2's: beside it)
                knob.tooltip: qsTr("How fast LFO 2 moves (♪: in note values)")
                syncButton.param: p.get("lfo2_sync")
                syncButton.tooltip: qsTr("Sync LFO 2 to the song's tempo")
            }
            CellKnob {
                id: safeBassKnob
                objectName: "safeBass"
                param: p.get("safe_bass")
                title: qsTr("Safe Bass")
                formatter: v => v <= param.minimum + 0.001 ? qsTr("Off") : param.format(v)
                tooltip: qsTr("Keep everything below this out of the effect (all the way down: off)")
            }
            EnvCell {
                id: envAmount
                objectName: "envAmount"
                dimmed: !editor.envOn
            }
            CellKnob {
                id: attackKnob
                objectName: "envAttack"
                param: p.get("env_attack")
                title: qsTr("Attack")
                tooltip: qsTr("How fast the follower rises")
                opacity: editor.envOn ? 1 : 0.55
            }
            CellKnob {
                id: releaseKnob
                objectName: "envRelease"
                param: p.get("env_release")
                title: qsTr("Release")
                tooltip: qsTr("How fast the follower falls")
                opacity: editor.envOn ? 1 : 0.55
            }
        }
    }
}
