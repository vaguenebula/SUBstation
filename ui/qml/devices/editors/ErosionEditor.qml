import QtQuick
import QtQuick.Controls
import SUBstation

// The Erosion's editor, laid out as Live 12.4's: Width and Stereo at the left;
// the display in the middle (ErosionGraph: the modulation's band over the
// input's spectrum, the output's as a line; drag its dot across for the
// Frequency and up and down for the Amount; Alt-drag or the wheel for the
// Width); Frequency, Amount and Noise Blend (the sine and noise glyphs beside
// it) at the right, with the modulation's left/right scope (ErosionScope).
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click gives
// its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    readonly property alias scope: scope

    // The knobs' cells: 66 px, room for the glyphs beside Noise Blend's knob, or wider if a font needs it for
    // the widest caption or readout (measured in their font: "Noise Blend" in most), with a pixel either side.
    readonly property int cellWidth: Math.max(66, Math.ceil(cellFont.widest) + 2)
    readonly property int graphWidth: graph.implicitWidth
    readonly property int spacing: 8
    readonly property int rowSpacing: 14
    // Noise Blend's weights, as the engine has them (equal power, exactly 1 and 0 at the ends).
    readonly property real sineWeight: graph.sineWeight
    readonly property real noiseWeight: graph.noiseWeight

    // The device's body: 8 + a column of knobs + SPACING + GRAPH_WIDTH + SPACING + two columns of
    // knobs 4 apart + 8, less the frame's border.
    implicitWidth: 8 + cellWidth + spacing + graphWidth + spacing + 2 * cellWidth + 4 + 8 - 2
    implicitHeight: 6 + Math.max(left.implicitHeight, right.implicitHeight, graph.implicitHeight) + 6

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["freq", "width", "amount", "blend", "stereo"]
    }

    // What the cells show at their widest, in EditorCaption's and EditorReadout's font: the captions, and the
    // knobs' readouts as patterns of their widest values, each "d" the font's widest digit (figures may be
    // proportional). Each measure reads `font` first: advanceWidth() alone doesn't make the font a dependency,
    // and it is set after a binding's first evaluation.
    FontMetrics {
        id: cellFont

        readonly property string widestDigit: {
            void font
            let widest = "0"
            for (const digit of "123456789") {
                if (cellFont.advanceWidth(digit) > cellFont.advanceWidth(widest))
                    widest = digit
            }
            return widest
        }
        readonly property real widest: {
            void font
            const captions = [qsTr("Width"), qsTr("Stereo"), qsTr("Frequency"), qsTr("Amount"), qsTr("Noise Blend"),
                              qsTr("L/R Mod")]
            const readouts = ["1d.dd kHz", "ddd Hz", "10.00 oct", "d.dd oct", "100 %", "dd %"]
            let most = 0
            for (const text of captions.concat(readouts.map(pattern => pattern.replace(/d/g, widestDigit))))
                most = Math.max(most, cellFont.advanceWidth(text))
            return most
        }

        font: Theme.uiFont(8)
    }

    Row {
        x: 8
        y: 6
        height: editor.height - 12
        spacing: editor.spacing

        Column {
            id: left
            spacing: editor.rowSpacing

            // Dimmed while the sine alone plays (it does nothing to it), still adjustable, as Live's: as
            // EditorKnob's disabled look (which animates its opacity).
            EditorKnob {
                objectName: "width"
                width: editor.cellWidth
                param: p.get("width")
                title: qsTr("Width")
                tooltip: qsTr("Filter Width: how wide the noise's band is, in octaves. Narrow, it wobbles like a "
                              + "tone; wide, it hisses over everything. No effect on the sine. Alt-drag or turn "
                              + "the wheel over the display")
                opacity: editor.noiseWeight > 0.001 ? 1 : 0.55
            }
            EditorKnob {
                objectName: "stereo"
                width: editor.cellWidth
                param: p.get("stereo")
                title: qsTr("Stereo")
                tooltip: qsTr("Stereo Width: from the same modulation on both sides (0 %) to independent noise "
                              + "and a sine a quarter cycle apart (100 %)")
            }
        }

        ErosionGraph {
            id: graph
            objectName: "erosionGraph"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            width: editor.graphWidth
            height: Math.max(implicitHeight, parent.height)

            HoverHandler {
                id: graphHover
            }
            ToolTip.visible: graphHover.hovered && !graph.dragging
            ToolTip.delay: 700
            ToolTip.text: qsTr("Drag: Frequency across, Amount up and down (Shift: finely). Alt-drag up and down "
                               + "or the wheel (Ctrl: finely): Filter Width. Behind it, the input's spectrum "
                               + "(filled) and the output's (line)")
        }

        Grid {
            id: right
            columns: 2
            columnSpacing: 4
            rowSpacing: editor.rowSpacing

            EditorKnob {
                id: freqKnob
                objectName: "freq"
                width: editor.cellWidth
                param: p.get("freq")
                title: qsTr("Frequency")
                tooltip: qsTr("Frequency: the sine's frequency, or the middle of the noise's band. Drag the "
                              + "display across")
            }
            EditorKnob {
                objectName: "amount"
                width: editor.cellWidth
                param: p.get("amount")
                title: qsTr("Amount")
                tooltip: qsTr("Amount: how far the modulation moves the short delay. High frequencies erode "
                              + "first. Drag the display up and down")
            }
            // Noise Blend with its glyphs, pictures as Live's (not buttons): each as bright as its source plays.
            Item {
                // The glyphs' middle: the knob's (EditorKnob is a Column: the caption, 1 px, the knob).
                readonly property real glyphMiddle: blendKnob.knob.y + blendKnob.knob.height / 2

                width: editor.cellWidth
                implicitHeight: blendKnob.implicitHeight
                height: implicitHeight

                EditorKnob {
                    id: blendKnob
                    objectName: "blend"
                    width: parent.width
                    param: p.get("blend")
                    title: qsTr("Noise Blend")
                    tooltip: qsTr("Noise Blend: from the sine (0 %) to filtered noise (100 %)")
                }
                Icon {
                    objectName: "sineGlyph"
                    name: "wave_sine"
                    size: 11
                    color: Theme.soloOn
                    x: 1
                    y: parent.glyphMiddle - height / 2
                    opacity: 0.3 + 0.7 * editor.sineWeight
                }
                Grid {  // a 3 x 3 checkerboard: Live's noise icon
                    objectName: "noiseGlyph"
                    columns: 3
                    x: parent.width - width - 1
                    y: parent.glyphMiddle - height / 2
                    opacity: 0.3 + 0.7 * editor.noiseWeight

                    Repeater {
                        model: 9
                        Rectangle {
                            required property int index
                            width: 3
                            height: 3
                            color: index % 2 === 0 ? Theme.accent : "transparent"
                        }
                    }
                }
            }
            // The scope, as tall as a knob's cell (never taller: the body's height is fixed).
            Column {
                width: editor.cellWidth
                spacing: 1

                EditorCaption {
                    id: scopeCaption
                    width: parent.width
                    text: qsTr("L/R Mod")
                }
                ErosionScope {
                    id: scope
                    objectName: "erosionScope"
                    // Its own size (its implicit one), or less to fit under the caption.
                    readonly property real side: Math.min(implicitWidth, freqKnob.height - scopeCaption.height - 1)
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: side
                    height: side
                    session: Session
                    trackId: editor.trackId
                    deviceId: editor.deviceId

                    HoverHandler {
                        id: scopeHover
                    }
                    ToolTip.visible: scopeHover.hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("The modulation, left against right: a line while both sides move alike; "
                                       + "a cloud (noise) or a circle (sine) as Stereo widens it")
                }
            }
        }
    }
}
