import QtQuick
import QtQuick.Controls
import SUBstation

// The Limiter's editor, laid out as Ableton's: the Gain knob (the Output's while Maximize is on) over
// the Maximize button; the display (LimiterGraph: the level over the last 1.5 s with the gain
// reduction from the top, In, GR and Out meters, and the line, the Ceiling or with Maximize the
// Threshold, dragged up and down or typed into its box at the top left); Release over Auto; then
// Lookahead, Mode, Routing and Link. Every control shows its parameter as it is now (its automation's
// value while that plays), sets it undoably, touches it when pressed, and right-click gives its menu.
// The columns are as wide as their texts need in the font the UI has (measured).
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    readonly property int gap: 10
    readonly property bool maximizeOn: p.get("maximize") ? p.get("maximize").value >= 0.5 : false
    readonly property bool autoOn: p.get("auto_release") ? p.get("auto_release").value >= 0.5 : false

    // The device's body: 8 + KNOB + GAP + GRAPH + GAP + KNOB + 8 + SIDE + 8.
    implicitWidth: 8 + knobWidth + gap + graph.implicitWidth + gap + knobWidth + 8 + sideWidth + 8
    implicitHeight: 6 + Math.max(gainColumn.minimumHeight, releaseColumn.minimumHeight, sideColumn.minimumHeight,
                                 graph.implicitHeight) + 6

    // The knobs' columns: 64 px, or wider where a knob's caption or a text its readout takes (EditorKnob's texts(),
    // as a Text lays them out: Gain and Output ±24 dB, Release 0.10 ms to 3.00 s) or Maximize's or Auto's text
    // needs it.
    EditorTextsWidth {
        id: knobTexts
        knobs: [gain, outputKnob, release]
    }
    readonly property int knobWidth: Math.ceil(Math.max(64, maximize.implicitWidth, autoRelease.implicitWidth,
                                                        knobTexts.needed))
    // The right column: 96 px, or wider where its captions, a list's longest name with its arrow, the Routing
    // buttons or Link's caption and box need it.
    EditorTextsWidth {
        id: sideCaptions
        texts: [qsTr("Lookahead"), qsTr("Mode")]
    }
    readonly property int sideWidth: Math.ceil(Math.max(96, sideCaptions.needed, lookahead.widest, mode.widest,
                                                        routingRow.widest,
                                                        linkCaption.implicitWidth + 2 + linkBox.width))

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["gain", "ceiling", "release", "auto_release", "lookahead", "mode", "routing", "link", "maximize",
              "threshold", "output"]
    }

    // Gain, or with Maximize the Output (the two crossfade; only the one fading in takes the mouse, so a
    // press mid-fade never turns the one going away), over Maximize (at the bottom, level with the
    // display's).
    Item {
        id: gainColumn

        readonly property real minimumHeight: gain.implicitHeight + 6 + maximize.implicitHeight

        x: 8
        y: 6
        width: editor.knobWidth
        height: Math.max(minimumHeight, editor.height - 12)

        Item {
            width: parent.width
            height: gain.implicitHeight

            EditorKnob {
                id: gain
                objectName: "gain"
                width: parent.width
                param: p.get("gain")
                title: qsTr("Gain")
                tooltip: qsTr("Gain: boosts or cuts the input before limiting. "
                              + "Turn it up to push the sound into the ceiling")
                knob.bipolar: true  // (±24 dB: 0 is the middle)
                opacity: editor.maximizeOn ? 0 : 1  // (EditorKnob's own Behavior fades it)
                visible: opacity > 0
                enabled: !editor.maximizeOn  // (its opacity is set here, so it doesn't dim)
            }
            EditorKnob {
                id: outputKnob
                objectName: "output"
                width: parent.width
                param: p.get("output")
                title: qsTr("Output")
                tooltip: qsTr("Output: where the loudest peaks come out with Maximize on (its ceiling)")
                opacity: editor.maximizeOn ? 1 : 0
                visible: opacity > 0
                enabled: editor.maximizeOn
            }
        }
        ParamButton {
            id: maximize
            objectName: "maximize"
            anchors.bottom: parent.bottom
            width: parent.width
            param: p.get("maximize")
            text: qsTr("Maximize")
            tooltip: qsTr("Maximize: loudness from one control. Lower the Threshold and everything comes up by as "
                          + "much; what reaches it comes out at the Output level")
        }
    }

    LimiterGraph {
        id: graph
        objectName: "limiterGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 8 + editor.knobWidth + editor.gap
        y: 6
        width: implicitWidth
        height: Math.max(implicitHeight, editor.height - 12)

        HoverHandler {
            id: graphHover
        }
        // (over the plot and the meters: the header has the line's box, with a tooltip of its own)
        ToolTip.visible: graphHover.hovered && graphHover.point.position.y > 24 && !graph.dragging
        ToolTip.delay: 700
        ToolTip.text: qsTr("Level over the last 1.5 s: input (grey, red where it goes over the line), output "
                           + "(light), gain reduction (orange, from the top). Drag the line to set the Ceiling "
                           + "(the Threshold with Maximize); double-click it for its default")
    }

    // The line's value, in the display's header (over it, so declared after it). The caption is as
    // wide as the wider of its two names (and 4 px), so the box stays put when Maximize swaps them; the
    // box as wide as the Ceiling's and the Threshold's texts need with the automation dot clear of them.
    EditorTextsWidth {
        id: lineNames
        font: lineCaption.font
        texts: [qsTr("Ceiling"), qsTr("Threshold")]
    }
    EditorBoxWidth {
        id: lineTexts
        params: [p.get("ceiling"), p.get("threshold")]
    }
    EditorCaption {
        id: lineCaption
        objectName: "lineCaption"
        x: graph.x + 6
        y: graph.y + 2
        width: lineNames.needed + 4
        height: 18
        horizontalAlignment: Text.AlignLeft
        verticalAlignment: Text.AlignVCenter
        text: editor.maximizeOn ? qsTr("Threshold") : qsTr("Ceiling")
    }
    ParamBox {
        id: lineBox
        objectName: "lineBox"
        x: lineCaption.x + lineCaption.width + 2
        y: graph.y + 2
        width: lineTexts.needed
        param: editor.maximizeOn ? p.get("threshold") : p.get("ceiling")
        step: 0.1
        decimals: 1
        defaultValue: param ? param.defaultValue : undefined
        sampleText: lineTexts.sample
        formatter: v => param ? param.format(v) : ""
        parser: text => param ? param.parse(text) : null
        tooltip: editor.maximizeOn
                 ? qsTr("Threshold: the level where limiting starts; the gain is Output − Threshold")
                 : qsTr("Ceiling: no peak comes out above it")
    }

    // Release over Auto. While Auto is on Release is dimmed, as Ableton greys it, and can still be set
    // (for when Auto is off again).
    Item {
        id: releaseColumn

        readonly property real minimumHeight: release.implicitHeight + 6 + autoRelease.implicitHeight

        x: graph.x + graph.width + editor.gap
        y: 6
        width: editor.knobWidth
        height: Math.max(minimumHeight, editor.height - 12)

        EditorKnob {
            id: release
            objectName: "release"
            width: parent.width
            param: p.get("release")
            title: qsTr("Release")
            opacity: editor.autoOn ? 0.55 : 1
            tooltip: qsTr("Release: how fast the gain comes back after a peak (Auto sets it while it is on; "
                          + "this one takes over when Auto is off)")
        }
        ParamButton {
            id: autoRelease
            objectName: "autoRelease"
            anchors.bottom: parent.bottom
            width: parent.width
            param: p.get("auto_release")
            text: qsTr("Auto")
            tooltip: qsTr("Auto release: quick after short peaks, slower while limiting goes on, so it neither "
                          + "pumps nor distorts")
        }
    }

    // Lookahead, Mode, Routing and Link, spread over the body's height.
    Column {
        id: sideColumn

        readonly property real content: lookaheadGroup.implicitHeight + modeGroup.implicitHeight
                                        + routingRow.implicitHeight + linkRow.implicitHeight
        readonly property real minimumHeight: content + 3 * 6

        x: releaseColumn.x + releaseColumn.width + 8
        y: 6
        width: editor.sideWidth
        spacing: Math.max(6, (editor.height - 12 - content) / 3)

        Column {
            id: lookaheadGroup
            width: parent.width

            EditorCaption {
                objectName: "lookaheadCaption"
                width: parent.width
                text: qsTr("Lookahead")
            }
            ParamChoice {
                id: lookahead
                objectName: "lookahead"
                // (the width its longest name and the arrow need)
                readonly property real widest: lookaheadNames.needed + button.leftPadding + button.rightPadding
                width: parent.width
                param: p.get("lookahead")
                tooltip: qsTr("Lookahead: how far ahead peaks are seen (the device's latency). Shorter is punchier "
                              + "but can distort lows")
            }
        }
        Column {
            id: modeGroup
            width: parent.width

            EditorCaption {
                objectName: "modeCaption"
                width: parent.width
                text: qsTr("Mode")
            }
            ParamChoice {
                id: mode
                objectName: "mode"
                readonly property real widest: modeNames.needed + button.leftPadding + button.rightPadding
                width: parent.width
                param: p.get("mode")
                tooltip: qsTr("Standard: no sample above the ceiling. Soft Clip: rounds peaks off as they near it, "
                              + "louder with some crunch. True Peak: no peak between samples above it either "
                              + "(for streaming)")
            }
        }
        Row {
            id: routingRow
            // (the two alike, as wide as the wider's text needs)
            readonly property real widest: 2 * Math.max(routingLR.implicitWidth, routingMS.implicitWidth) + spacing
            spacing: 2

            ParamButton {
                id: routingLR
                objectName: "routingLR"
                width: (editor.sideWidth - 2) / 2
                param: p.get("routing")
                choice: 0
                text: qsTr("L/R")
                tooltip: qsTr("L/R: limits left and right")
            }
            ParamButton {
                id: routingMS
                objectName: "routingMS"
                width: (editor.sideWidth - 2) / 2
                param: p.get("routing")
                choice: 1
                text: qsTr("M/S")
                tooltip: qsTr("M/S: limits the middle and the sides, so a loud centre needn't pull the sides down")
            }
        }
        Row {
            id: linkRow
            spacing: 2

            EditorCaption {
                id: linkCaption
                objectName: "linkCaption"
                width: editor.sideWidth - linkBox.width - 2
                height: linkBox.height
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Link")
            }
            ParamBox {
                id: linkBox
                objectName: "link"
                width: linkTexts.needed
                param: p.get("link")
                step: 1
                decimals: 0
                defaultValue: param ? param.defaultValue : undefined
                sampleText: linkTexts.sample
                formatter: v => param ? param.format(v) : ""
                parser: text => param ? param.parse(text) : null
                tooltip: qsTr("Link: how much of one channel's gain reduction the other shares (100 %: both alike, "
                              + "0 %: each its own)")
            }
        }
    }
    // Link's texts (0 to 100 %), as its box draws them; the lists' names, in their font.
    EditorBoxWidth {
        id: linkTexts
        params: [p.get("link")]
    }
    EditorTextsWidth {
        id: lookaheadNames
        font: lookahead.button.font
        texts: lookahead.names
    }
    EditorTextsWidth {
        id: modeNames
        font: mode.button.font
        texts: mode.names
    }
}
