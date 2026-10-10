import QtQuick
import QtQuick.Controls
import SUBstation

// The Saturator's editor, laid out as Live 12.1's Saturator with its expanded
// view open beside the front panel: Drive, the Type and the DC and HQ switches;
// the shaper curve (SaturatorCurve: the signal lit on it, In and Out strips;
// drag it for Drive) with Post Clip under it; Output and Dry/Wet. Then Color:
// its switch over its pre-shaper EQ on the input and output spectra
// (SaturatorColorGraph; drag its handles) and its four knobs (Amt Lo, Freq,
// Width, Amt Hi: Live 12.1's names). Last the curve's own controls: the
// Waveshaper's six, or the Bass Shaper's Threshold. Every control shows its
// parameter as it is now (its automation's value while that plays), sets it
// undoably, touches it when pressed, and right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias curve: curve
    readonly property alias colorGraph: colorGraph

    // The Type list's index, and what it shows (which entries are the Bass Shaper and the Waveshaper is the
    // engine's, through the curve).
    readonly property int type: p.get("type") ? p.get("type").index : 0
    readonly property bool waveshaper: type === curve.waveshaperType
    readonly property bool bass: type === curve.bassShaperType
    readonly property bool colorOn: p.get("color") ? p.get("color").value >= 0.5 : false

    // The Type list as wide as its longest name with its arrow (and the front panel with it).
    readonly property real typeListWidth: {
        const names = typeChoice.names
        let widest = 0
        for (let i = 0; i < names.length; ++i)
            widest = Math.max(widest, listFont.advanceWidth(names[i]))
        return Math.ceil(widest + typeChoice.button.leftPadding + typeChoice.button.rightPadding) + 2
    }

    // Columns 8 px apart: the front panel (Drive, the curve, Output and Dry/Wet), a line, Color, a line, the
    // shaper's controls.
    implicitWidth: shaperSection.x + shaperSection.width + 8
    implicitHeight: 6 + Math.max(front.implicitHeight, curveColumn.implicitHeight, levels.implicitHeight,
                                 colorSection.implicitHeight, shaperSection.implicitHeight) + 6

    FontMetrics {
        id: listFont
        font: typeChoice.button.font
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["drive", "type", "threshold", "output", "mix", "clip", "color", "base", "freq", "width", "depth", "dc",
            "hq", "ws_drive", "ws_lin", "ws_curve", "ws_damp", "ws_depth", "ws_period"]
    }

    // --- The front panel ------------------------------------------------------------------

    Item {
        id: front
        objectName: "front"
        x: 8
        y: 6
        width: Math.max(84, editor.typeListWidth)
        height: editor.height - 12
        implicitHeight: drive.height + 4 + typeChoice.height + 4 + switches.height

        EditorKnob {
            id: drive
            objectName: "drive"
            width: parent.width
            size: 40
            param: p.get("drive")
            title: qsTr("Drive")
            tooltip: qsTr("Drive: the level into the curve. Turn it up to saturate more; the curve shows how far")
            knob.bipolar: true
        }
        ParamChoice {
            id: typeChoice
            objectName: "type"
            y: drive.height + 4
            width: parent.width
            param: p.get("type")
            tooltip: qsTr("The shaping curve: Analog Clip, Soft Sine, Bass Shaper (linear below its Threshold), " +
                          "Medium Curve, Hard Curve, Sinoid Fold (folds back over full scale), Digital Clip, or " +
                          "the Waveshaper's own")
        }
        Row {
            id: switches
            anchors.bottom: parent.bottom
            spacing: 4

            // On whole pixels (the front panel's width can be odd), so both buttons' inner edges are sharp.
            ParamButton {
                id: dc
                objectName: "dc"
                width: Math.floor((front.width - switches.spacing) / 2)
                param: p.get("dc")
                text: qsTr("DC")
                tooltip: qsTr("DC: removes DC offset from the input before it is shaped")
            }
            ParamButton {
                objectName: "hq"
                width: front.width - switches.spacing - dc.width
                param: p.get("hq")
                text: qsTr("HQ")
                tooltip: qsTr("Hi-Quality: shapes and clips at 4× the sample rate, so loud high sounds alias far " +
                              "less (more CPU, %1 samples of latency)").arg(curve.hqLatency)
            }
        }
    }

    Item {
        id: curveColumn
        objectName: "curveColumn"
        x: front.x + front.width + 8
        y: 6
        width: 160
        height: editor.height - 12
        implicitHeight: curve.implicitHeight + 4 + clip.height

        SaturatorCurve {
            id: curve
            objectName: "saturatorCurve"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            width: parent.width
            height: Math.max(implicitHeight, parent.height - 4 - clip.height)

            HoverHandler {
                id: curveHover
            }
            ToolTip.visible: curveHover.hovered && !curveHover.point.pressedButtons
            ToolTip.delay: 700
            ToolTip.text: qsTr("The shaping curve: input across, output up. The light shows how far the signal " +
                               "reaches. Drag up and down for Drive; across for the Threshold (Bass Shaper) or " +
                               "Curve (Waveshaper). Double-click: Drive to 0 dB")
        }
        // Post Clip Mode, under the curve as in Live.
        ParamChoice {
            id: clip
            objectName: "clip"
            anchors.bottom: parent.bottom
            width: parent.width
            param: p.get("clip")
            tooltip: qsTr("Post Clip: clips the output, dry and wet together, softly (the Analog Clip curve) or " +
                          "hard, so it never goes over the Output level (with HQ, bright sound can a little)")
        }
    }

    // Output at the top, Dry/Wet at the bottom: their values on the line the other sections' end on.
    Item {
        id: levels
        objectName: "levels"
        x: curveColumn.x + curveColumn.width + 8
        y: 6
        width: 62
        height: editor.height - 12
        implicitHeight: outputKnob.height + 6 + mixKnob.height

        EditorKnob {
            id: outputKnob
            objectName: "output"
            width: parent.width
            param: p.get("output")
            title: qsTr("Output")
            tooltip: qsTr("Output: the level out of the device")
        }
        EditorKnob {
            id: mixKnob
            objectName: "mix"
            anchors.bottom: parent.bottom
            width: parent.width
            param: p.get("mix")
            title: qsTr("Dry/Wet")
            tooltip: qsTr("Dry/Wet: the shaped sound blended with the input")
        }
    }

    EditorDivider {
        id: colorDivider
        x: levels.x + levels.width + 8
    }

    // --- Color --------------------------------------------------------------------------------

    Item {
        id: colorSection
        objectName: "colorSection"
        x: colorDivider.x + 9
        y: 6
        width: 220
        height: editor.height - 12
        implicitHeight: colorSwitch.height + 4 + colorGraph.implicitHeight + 4 + colorKnobs.height

        // The switch over the graph (not on it: it would hide a handle dragged up there).
        ParamButton {
            id: colorSwitch
            objectName: "color"
            width: 44
            param: p.get("color")
            text: qsTr("Color")
            tooltip: qsTr("Color: two filters around the curve. What they boost is driven harder (then turned " +
                          "back down); what they cut stays clean (then turned back up)")
        }
        SaturatorColorGraph {
            id: colorGraph
            objectName: "saturatorColor"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            y: colorSwitch.height + 4
            width: parent.width
            height: Math.max(implicitHeight, parent.height - y - 4 - colorKnobs.height)

            HoverHandler {
                id: colorHover
            }
            ToolTip.visible: colorHover.hovered && !colorHover.point.pressedButtons
            ToolTip.delay: 700
            ToolTip.text: qsTr("Color's pre-shaper EQ over the input (filled) and output (line) spectra. Drag " +
                               "the dots: Amt Lo's up and down; the band's across for Freq and up and down for " +
                               "Amt Hi")
        }
        Row {
            id: colorKnobs
            anchors.bottom: parent.bottom
            spacing: 4

            EditorKnob {
                objectName: "base"
                size: 24
                param: p.get("base")
                title: qsTr("Amt Lo")
                tooltip: qsTr("Amt Lo: more (+) or less (−) saturation of the lows")
                knob.bipolar: true
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "freq"
                size: 24
                param: p.get("freq")
                title: qsTr("Freq")
                tooltip: qsTr("Freq: the centre of Color's band")
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "width"
                size: 24
                param: p.get("width")
                title: qsTr("Width")
                tooltip: qsTr("Width: how wide Color's band is")
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "depth"
                size: 24
                param: p.get("depth")
                title: qsTr("Amt Hi")
                tooltip: qsTr("Amt Hi: more (+) or less (−) saturation of the band around Freq")
                knob.bipolar: true
                opacity: editor.colorOn ? 1 : 0.55
            }
        }
    }

    EditorDivider {
        id: shaperDivider
        x: colorSection.x + colorSection.width + 8
    }

    // --- The curve's own controls: the Waveshaper's, or the Bass Shaper's Threshold -----------

    Item {
        id: shaperSection
        objectName: "shaperSection"
        x: shaperDivider.x + 9
        y: 6
        width: 164
        height: editor.height - 12
        implicitHeight: shaperTitle.height + 4 + Math.max(waveshaperSection.implicitHeight, bassSection.implicitHeight)

        EditorCaption {
            id: shaperTitle
            objectName: "shaperTitle"
            width: parent.width
            height: 16
            verticalAlignment: Text.AlignVCenter
            font: Theme.uiFont(8, true)
            text: editor.bass ? qsTr("Bass Shaper") : qsTr("Waveshaper")
            color: editor.bass || editor.waveshaper ? Theme.accent : Theme.textDim
            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
        }

        // The Waveshaper's six: Drive, Lin and Curve under the title, Damp, Depth and Period at the bottom.
        Item {
            id: waveshaperSection
            objectName: "waveshaperSection"
            y: shaperTitle.height + 4
            width: parent.width
            height: parent.height - y
            implicitHeight: wsTop.height + 4 + wsBottom.height
            opacity: editor.bass ? 0 : 1
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 150
                    easing.type: Easing.InOutQuad
                }
            }

            Row {
                id: wsTop
                spacing: 4

                EditorKnob {
                    objectName: "ws_drive"
                    size: 24
                    param: p.get("ws_drive")
                    title: qsTr("Drive")
                    tooltip: qsTr("Drive: how much of the Waveshaper's curve is heard (0: none)")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
                EditorKnob {
                    objectName: "ws_lin"
                    size: 24
                    param: p.get("ws_lin")
                    title: qsTr("Lin")
                    tooltip: qsTr("Lin: the curve's straight part (its slope)")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
                EditorKnob {
                    objectName: "ws_curve"
                    size: 24
                    param: p.get("ws_curve")
                    title: qsTr("Curve")
                    tooltip: qsTr("Curve: a cubic bend; mostly third harmonics")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
            }
            Row {
                id: wsBottom
                anchors.bottom: parent.bottom
                spacing: 4

                EditorKnob {
                    objectName: "ws_damp"
                    size: 24
                    param: p.get("ws_damp")
                    title: qsTr("Damp")
                    tooltip: qsTr("Damp: flattens quiet signal near the middle, like a very fast gate")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
                EditorKnob {
                    objectName: "ws_depth"
                    size: 24
                    param: p.get("ws_depth")
                    title: qsTr("Depth")
                    tooltip: qsTr("Depth: ripples of a sine over the curve")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
                EditorKnob {
                    objectName: "ws_period"
                    size: 24
                    param: p.get("ws_period")
                    title: qsTr("Period")
                    tooltip: qsTr("Period: how many ripples")
                    opacity: editor.waveshaper ? 1 : 0.55
                }
            }
        }

        Column {
            id: bassSection
            objectName: "bassSection"
            y: shaperTitle.height + 4
            width: parent.width
            spacing: 4
            opacity: editor.bass ? 1 : 0
            visible: opacity > 0
            Behavior on opacity {
                NumberAnimation {
                    duration: 150
                    easing.type: Easing.InOutQuad
                }
            }

            EditorKnob {
                objectName: "threshold"
                anchors.horizontalCenter: parent.horizontalCenter
                width: 62
                param: p.get("threshold")
                title: qsTr("Threshold")
                tooltip: qsTr("Threshold: the Bass Shaper is linear below it and saturates above it; at 0 dB it " +
                              "clips hard")
            }
            EditorReadout {
                objectName: "bassHint"
                width: parent.width
                color: Theme.textDim
                text: qsTr("linear below, saturating above")
            }
        }
    }
}
