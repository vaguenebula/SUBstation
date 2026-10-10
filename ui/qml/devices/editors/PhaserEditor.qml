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
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property int margin: 8
    readonly property int cell: 52      // a knob's cell (an EditorKnob's width at the house size)
    readonly property int knobSize: 34
    readonly property int gap: 4        // between cells
    readonly property int sectionGap: 10
    readonly property int tabWidth: 58
    readonly property int pairWidth: 2 * cell + gap        // the mode's and the LFO's sections
    readonly property int tripleWidth: 3 * cell + 2 * gap  // the globals and the extra section
    readonly property int graphWidth: 240

    // Where each section starts.
    readonly property int modeX: margin + tabWidth + 8
    readonly property int graphX: modeX + pairWidth + sectionGap
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
    implicitHeight: 6 + Math.max(rowsHeight, time.implicitHeight + 14, graph.implicitHeight, 3 * 16 + 8) + 6

    function on(id) {
        const param = p.get(id)
        return param ? param.value >= 0.5 : false
    }

    function setExpanded(open) {
        DeviceViews.setValue(deviceId, "expanded", open)
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
        ParamButton {
            id: syncButton
            objectName: synced.buttonName
            x: 40
            y: 0
            width: 12
            height: 12
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
        property bool dimmed: false

        width: editor.cell
        spacing: 1

        ParamButton {
            objectName: "envOn"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 40
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
        width: editor.pairWidth
        height: editor.height

        // The Phaser's: its notches, where they are, how far apart, and what the modulation moves.
        Grid {
            id: phaserKnobs
            objectName: "phaserKnobs"
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

            EditorKnob {
                id: notches
                objectName: "notches"
                size: editor.knobSize
                step: 1
                param: p.get("notches")
                title: qsTr("Notches")
                tooltip: qsTr("How many all-pass filters, one notch each")
            }
            EditorKnob {
                objectName: "center"
                size: editor.knobSize
                param: p.get("center")
                title: qsTr("Center")
                tooltip: qsTr("Where the notches sit")
            }
            EditorKnob {
                objectName: "spread"
                size: editor.knobSize
                param: p.get("spread")
                title: qsTr("Spread")
                tooltip: qsTr("How far apart the notches are (the filters' Q)")
            }
            EditorKnob {
                objectName: "blend"
                size: editor.knobSize
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
                    objectName: "spinOn"
                    width: editor.cell
                    param: p.get("spin_on")
                    text: qsTr("Spin")
                    tooltip: qsTr("Spin instead of a fixed phase")
                }
            }
        }
        EditorKnob {
            id: duty
            objectName: "duty"
            size: editor.knobSize
            param: p.get("lfo_duty")
            knob.bipolar: true
            title: qsTr("Duty")
            tooltip: qsTr("Bends the shape: a rectangle's width, the others' skew")
        }
        EditorKnob {
            objectName: "phaseSpin"
            size: editor.knobSize
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

        EditorKnob {
            objectName: "amount"
            size: editor.knobSize
            param: p.get("amount")
            title: qsTr("Amount")
            tooltip: qsTr("How far the LFO and the envelope move the sweep")
        }
        EditorKnob {
            objectName: "feedback"
            size: editor.knobSize
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
        EditorKnob {
            objectName: "warmth"
            size: editor.knobSize
            param: p.get("warmth")
            title: qsTr("Warmth")
            tooltip: qsTr("A little saturation and darkening of the effect")
        }
        EditorKnob {
            objectName: "output"
            size: editor.knobSize
            param: p.get("output")
            title: qsTr("Output")
            tooltip: qsTr("Output level")
        }
        EditorKnob {
            objectName: "mix"
            size: editor.knobSize
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

            EditorKnob {
                objectName: "lfo2Mix"
                size: editor.knobSize
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
            EditorKnob {
                objectName: "safeBass"
                size: editor.knobSize
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
            EditorKnob {
                objectName: "envAttack"
                size: editor.knobSize
                param: p.get("env_attack")
                title: qsTr("Attack")
                tooltip: qsTr("How fast the follower rises")
                opacity: editor.envOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "envRelease"
                size: editor.knobSize
                param: p.get("env_release")
                title: qsTr("Release")
                tooltip: qsTr("How fast the follower falls")
                opacity: editor.envOn ? 1 : 0.55
            }
        }
    }
}
