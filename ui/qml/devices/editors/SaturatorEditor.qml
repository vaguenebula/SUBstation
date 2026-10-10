import QtQuick
import QtQuick.Controls
import SUBstation

// The Saturator's editor, laid out as Live 12.1's Saturator with its expanded
// view open beside the front panel: Drive, the Type and the DC and HQ switches;
// the shaper curve (SaturatorCurve: the signal lit on it, In and Out strips;
// drag it for Drive) with Post Clip under it; Output and Dry/Wet. Then Color's
// pre-shaper EQ over the input and output spectra (SaturatorColorGraph; drag
// its handles) with its switch and four knobs, and the curve's own controls:
// the Waveshaper's six, or the Bass Shaper's Threshold. Every control shows its
// parameter as it is now (its automation's value while that plays), sets it
// undoably, touches it when pressed, and right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias curve: curve
    readonly property alias colorGraph: colorGraph

    // The Type list's index (2 Bass Shaper, 7 Waveshaper), and what it shows.
    readonly property int type: p.get("type") ? p.get("type").index : 0
    readonly property bool waveshaper: type === 7
    readonly property bool bass: type === 2
    readonly property bool colorOn: p.get("color") ? p.get("color").value >= 0.5 : false

    // Columns: the front panel (Drive, the curve, Output and Dry/Wet), a line, Color, a line, the shaper's controls.
    implicitWidth: 756
    implicitHeight: 6 + Math.max(front.implicitHeight, curveColumn.implicitHeight, levels.implicitHeight,
                                 colorSection.implicitHeight, shaperSection.implicitHeight) + 6

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
        x: 8
        y: 6
        width: 84
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
        }
        ParamChoice {
            id: typeChoice
            objectName: "type"
            y: drive.height + 4
            width: parent.width
            param: p.get("type")
            tooltip: qsTr("The shaping curve: Analog Clip, Soft Sine, Bass Shaper (linear below its Threshold), Medium Curve, Hard Curve, Sinoid Fold (folds back over full scale), Digital Clip, or the Waveshaper's own")
        }
        Row {
            id: switches
            anchors.bottom: parent.bottom
            spacing: 4

            ParamButton {
                objectName: "dc"
                width: 40
                param: p.get("dc")
                text: qsTr("DC")
                tooltip: qsTr("DC: removes DC offset from the input before it is shaped")
            }
            ParamButton {
                objectName: "hq"
                width: 40
                param: p.get("hq")
                text: qsTr("HQ")
                tooltip: qsTr("Hi-Quality: shapes at 4× the sample rate, so loud high sounds alias far less (more CPU, 36 samples of latency)")
            }
        }
    }

    Item {
        id: curveColumn
        x: 100
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
            ToolTip.text: qsTr("The shaping curve: input across, output up. The light shows how far the signal reaches. Drag up and down for Drive; across for the Threshold (Bass Shaper) or Curve (Waveshaper). Double-click: Drive to 0 dB")
        }
        // Post Clip Mode, under the curve as in Live.
        ParamChoice {
            id: clip
            objectName: "clip"
            anchors.bottom: parent.bottom
            width: parent.width
            param: p.get("clip")
            tooltip: qsTr("Post Clip: clips the output softly (the Analog Clip curve) or hard, so it never goes over the Output level")
        }
    }

    // Output at the top, Dry/Wet at the bottom: their values on the line the other sections' end on.
    Item {
        id: levels
        x: 268
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

    Rectangle {
        x: 338
        y: 6
        width: 1
        height: editor.height - 12
        color: Theme.border
    }

    // --- Color --------------------------------------------------------------------------------

    Item {
        id: colorSection
        objectName: "colorSection"
        x: 347
        y: 6
        width: 220
        height: editor.height - 12
        implicitHeight: colorGraph.implicitHeight + 4 + colorKnobs.height

        SaturatorColorGraph {
            id: colorGraph
            objectName: "saturatorColor"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            width: parent.width
            height: Math.max(implicitHeight, parent.height - 4 - colorKnobs.height)

            HoverHandler {
                id: colorHover
            }
            ToolTip.visible: colorHover.hovered && !colorHover.point.pressedButtons
            ToolTip.delay: 700
            ToolTip.text: qsTr("Color's pre-shaper EQ over the input (filled) and output (line) spectra. Drag the dots: Base up and down; the peak across for Freq and up and down for Depth")
        }
        ParamButton {
            objectName: "color"
            x: 4
            y: 4
            width: 44
            param: p.get("color")
            text: qsTr("Color")
            tooltip: qsTr("Color: two filters around the curve. What they boost is driven harder (then turned back down); what they cut stays clean (then turned back up)")
        }
        Row {
            id: colorKnobs
            anchors.bottom: parent.bottom
            spacing: 4

            EditorKnob {
                objectName: "base"
                size: 24
                param: p.get("base")
                title: qsTr("Base")
                tooltip: qsTr("Base: more (+) or less (−) saturation of the lows")
                knob.bipolar: true
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "freq"
                size: 24
                param: p.get("freq")
                title: qsTr("Freq")
                tooltip: qsTr("Freq: the centre of Color's second filter")
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "width"
                size: 24
                param: p.get("width")
                title: qsTr("Width")
                tooltip: qsTr("Width: how wide a band Color's second filter takes")
                opacity: editor.colorOn ? 1 : 0.55
            }
            EditorKnob {
                objectName: "depth"
                size: 24
                param: p.get("depth")
                title: qsTr("Depth")
                tooltip: qsTr("Depth: more (+) or less (−) saturation around Freq")
                knob.bipolar: true
                opacity: editor.colorOn ? 1 : 0.55
            }
        }
    }

    Rectangle {
        x: 575
        y: 6
        width: 1
        height: editor.height - 12
        color: Theme.border
    }

    // --- The curve's own controls: the Waveshaper's, or the Bass Shaper's Threshold -----------

    Item {
        id: shaperSection
        objectName: "shaperSection"
        x: 584
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
                tooltip: qsTr("Threshold: the Bass Shaper is linear below it and saturates above it; at 0 dB it clips hard")
            }
            EditorReadout {
                width: parent.width
                color: Theme.textDim
                text: qsTr("linear below, saturating above")
            }
        }
    }
}
