import QtQuick
import QtQuick.Controls
import SUBstation

// The Spectral Compressor's editor: the thresholds' controls at the left (Threshold, Ratio, Below and
// Upward over Tilt, Knee, Range and Smoothing), at the right the Focus band's edges as values (Focus Low
// over Focus High), then time and output (Attack, Release and Stereo Link over Dry/Wet, Output and Delta),
// and between them SpectralGraph: the spectrum in and out, the thresholds (drag them; tilt them by the
// orange line's end handles), the Focus band's edges (dragged too), and what each frequency is turned down
// (from the top) or brought up (from the bottom), as it plays. The Sidechain badge over the display is lit
// while another track keys it; a click asks the frame for the sidechain menu, under the badge. Below is
// dimmed (still settable) while Upward is 1:1, when it does nothing. Every control shows its parameter as it
// is now, sets it undoably, touches it when pressed, and right-click gives its menu. Everything with text is as
// wide as the font makes it: the knobs' cells their widest caption or readout (64 px at least), the Focus
// column its widest value clear of the automation dot and its captions, Delta its text, the display's header
// clear of the Sidechain badge.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    // The badge was clicked: the device's frame shows its sidechain menu under it (`from`).
    signal sidechainMenuRequested(var from)

    // A knob's cell: as wide as the widest caption or readout a knob shows ("Stereo Link", "-6.0 dB/oct"), 64 px
    // at least.
    readonly property int cellWidth: Math.max(64, Math.ceil(knobTexts.implicitWidth))
    // The Focus column: its value boxes wide enough for the widest value ("20.00 kHz", or "14.44 kHz" where the
    // 4 is the widest figure), centred with 11 px either side, 2 px clear of the automation dot (drawn at x
    // 3.5..8.5 in the box); and its captions.
    readonly property int focusWidth: Math.max(cellWidth, Math.ceil(figures.advance(focusSample)) + 2 * 11,
                                               Math.ceil(focusCaptions.implicitWidth))
    // The Focus boxes' widest text: of the forms their values take, "20 Hz" to "20.00 kHz" ("1000 Hz": 999.6).
    readonly property string focusSample: {
        const high = p.get("focus_hi")
        return figures.widestOf(["## Hz", "### Hz", "1000 Hz", "#.## kHz", "1#.## kHz"]
                                    .map(pattern => figures.sample(pattern))
                                    .concat(high ? [high.format(high.maximum)] : []))
    }
    readonly property int leftWidth: 4 * cellWidth
    readonly property int rightWidth: focusWidth + 3 * cellWidth  // the Focus column and three knobs
    readonly property int gap: 10
    readonly property int graphWidth: graph.implicitWidth

    // The device's body: the knobs either side of the display, 10 px from it, and the margins.
    implicitWidth: 8 + leftWidth + gap + graphWidth + gap + rightWidth + 8
    implicitHeight: 6 + Math.max(left.implicitHeight, right.implicitHeight, graph.implicitHeight) + 6

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["threshold", "ratio", "below", "upward", "tilt", "knee", "range", "smooth", "focus_lo", "focus_hi",
              "attack", "release", "link", "mix", "output", "delta"]
    }

    // A parameter's default, as its value reads ("-18.0 dB"), for the tooltips.
    function defaultText(id) {
        const param = p.get(id)
        return param ? param.format(param.defaultValue) : ""
    }

    // The captions' and readouts' figures (the Focus boxes' too). A FontMetrics measures the font's hinted
    // advances, as the Focus boxes draw their text (ValueBoxItem, with QPainter); Qt Quick text's design
    // advances can be narrower (Windows' Segoe UI at 8 pt: "20.00 kHz" is 49 px drawn, 47 by design).
    FontMetrics {
        id: figures

        // The widest figure (figures may be proportional; read in a binding that names the font, so it is
        // worked out again once the font is set).
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
        // `text`'s advance, and of `texts` the one with the widest (the font followed, as above).
        function advance(text) {
            void font
            return advanceWidth(text)
        }
        function widestOf(texts) {
            return texts.reduce((most, text) => advance(text) > advance(most) ? text : most, "")
        }

        font: Theme.uiFont(8)
    }

    // The widest of `lines` as a Text lays them out in its font (a caption or a readout is as wide as its
    // advance and whatever of its last glyph reaches past it): hidden, a line each.
    component Widest: Text {
        property var lines: []
        visible: false
        textFormat: Text.PlainText
        text: lines.join("\n")
    }
    // What the knobs show at their widest: their captions, and each readout's widest form, every figure the
    // font's widest (the levels' "-##.# dB", the ratios' "##.#:1", Tilt's "-#.# dB/oct", the percentages, the
    // times' "#.## s", "### ms", "1000 ms" (999.6 rounded) and "#.# ms"). In the captions' and readouts' font.
    Widest {
        id: knobTexts
        font: Theme.uiFont(8)
        lines: [thresholdKnob, ratioKnob, belowKnob, upwardKnob, tiltKnob, kneeKnob, rangeKnob, smoothKnob, attackKnob,
                releaseKnob, linkKnob, mixKnob, outputKnob]
            .map(knob => knob.title)
            .concat(["-##.# dB", "##.#:1", "-#.# dB/oct", "100 %", "#.## s", "### ms", "1000 ms", "#.# ms"]
                        .map(pattern => figures.sample(pattern)))
    }
    // The Focus column's captions.
    Widest {
        id: focusCaptions
        font: Theme.uiFont(8)
        renderType: Text.NativeRendering
        lines: [focusLow.title, focusHigh.title]
    }

    // A Focus edge's cell: its name over a value box, level with the knobs beside it.
    component FocusCell: Item {
        id: cell

        property string paramId: ""
        property string title: ""
        property string tooltip: ""

        width: editor.focusWidth
        height: attackKnob.height

        EditorCaption {
            id: caption
            width: parent.width
            text: cell.title
            elide: Text.ElideRight
        }
        ParamBox {
            objectName: cell.paramId
            y: caption.height + attackKnob.spacing + (attackKnob.knob.height - height) / 2
            width: parent.width
            param: p.get(cell.paramId)
            logScale: true
            decimals: 0
            defaultValue: param ? param.defaultValue : 0
            formatter: v => param ? param.format(v) : ""
            parser: text => param ? param.parse(text) : null
            sampleText: editor.focusSample
            tooltip: cell.tooltip
        }
    }

    // The curve: the thresholds and their ratios, then their shape.
    Grid {
        id: left
        x: 8
        y: 6
        columns: 4
        columnSpacing: 0
        rowSpacing: 8

        EditorKnob {
            id: thresholdKnob
            objectName: "threshold"
            width: editor.cellWidth
            param: p.get("threshold")
            title: qsTr("Threshold")
            tooltip: qsTr("Threshold: each frequency louder than this is turned down by Ratio. Pink noise reads "
                          + "its own level at every frequency")
        }
        EditorKnob {
            id: ratioKnob
            objectName: "ratio"
            width: editor.cellWidth
            param: p.get("ratio")
            title: qsTr("Ratio")
            tooltip: qsTr("Ratio: how hard each frequency above the threshold is pushed down")
        }
        EditorKnob {
            id: belowKnob
            objectName: "below"
            width: editor.cellWidth
            param: p.get("below")
            title: qsTr("Below")
            // (Settable, dimmed while Upward is 1:1: it does nothing then.)
            opacity: p.get("upward") && p.get("upward").value > 1.001 ? 1 : 0.55
            tooltip: qsTr("Below: each frequency quieter than this is brought up by Upward (never above "
                          + "Threshold; nothing while Upward is 1:1)")
        }
        EditorKnob {
            id: upwardKnob
            objectName: "upward"
            width: editor.cellWidth
            param: p.get("upward")
            title: qsTr("Upward")
            tooltip: qsTr("Upward: how far each frequency under Below is brought up towards it (1:1: not at all)")
        }
        EditorKnob {
            id: tiltKnob
            objectName: "tilt"
            width: editor.cellWidth
            param: p.get("tilt")
            title: qsTr("Tilt")
            knob.bipolar: true
            tooltip: qsTr("Tilt: turns both thresholds about 1 kHz, in dB per octave, against a pink spectrum "
                          + "(0: they follow pink noise; up: the highs are let louder)")
        }
        EditorKnob {
            id: kneeKnob
            objectName: "knee"
            width: editor.cellWidth
            param: p.get("knee")
            title: qsTr("Knee")
            tooltip: qsTr("Knee: how gradually the compression starts around each threshold")
        }
        EditorKnob {
            id: rangeKnob
            objectName: "range"
            width: editor.cellWidth
            param: p.get("range")
            title: qsTr("Range")
            tooltip: qsTr("Range: the most any frequency is turned down or up")
        }
        EditorKnob {
            id: smoothKnob
            objectName: "smooth"
            width: editor.cellWidth
            param: p.get("smooth")
            title: qsTr("Smoothing")
            tooltip: qsTr("Smoothing: how wide a band each frequency's level is measured over: 0 each bin "
                          + "alone (most selective), 100 two octaves (gentlest)")
        }
    }

    EditorDivider {
        x: left.x + editor.leftWidth + editor.gap / 2
    }

    SpectralGraph {
        id: graph
        objectName: "spectralGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 8 + editor.leftWidth + editor.gap
        y: 6
        width: editor.width - x - editor.gap - editor.rightWidth - 8
        height: Math.max(implicitHeight, editor.height - 12)
        // The header's readout starts 8 px clear of the Sidechain badge over its left.
        headerLeft: Math.ceil(badge.x - x + badge.width) + 8

        HoverHandler {
            id: graphHover
        }
        ToolTip.visible: graphHover.hovered && !graphHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: [
            qsTr("Spectrum in (filled) and out (line). The orange line is the threshold: drag it, or its end "
                 + "handles to tilt it. Orange from the top: how far each frequency is turned down; green from "
                 + "the bottom: how far it is brought up"),
            qsTr("Threshold: drag up or down (Shift: finely; double-click: back to %1)")
                .arg(editor.defaultText("threshold")),
            qsTr("Tilt: drag to turn both thresholds about 1 kHz (double-click: level with pink)"),
            qsTr("Tilt: drag to turn both thresholds about 1 kHz (double-click: level with pink)"),
            qsTr("Below: drag up or down; frequencies under the green line are brought up (double-click: back "
                 + "to %1)").arg(editor.defaultText("below")),
            qsTr("Focus Low: drag sideways; nothing below it is changed (double-click: all the way down)"),
            qsTr("Focus High: drag sideways; nothing above it is changed (double-click: all the way up)")
        ][graph.hoveredHandle] || ""
    }

    // Over the display's top left: lit while a sidechain keys the gains; a click opens the sidechain menu.
    Rectangle {
        id: badge
        objectName: "sidechainBadge"

        readonly property bool lit: graph.keyed

        x: graph.x + 4
        y: graph.y + 2
        width: badgeText.implicitWidth + 10
        height: 12
        radius: 3
        color: lit ? Theme.accent : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0)
        border.width: lit ? 0 : 1
        border.color: Theme.textDisabled
        Behavior on color {
            ColorAnimation {
                duration: 150
            }
        }

        Text {
            id: badgeText
            anchors.centerIn: parent
            text: qsTr("Sidechain")
            font: Theme.uiFont(7)
            color: badge.lit ? Theme.accentText : Theme.textDim
        }
        MouseArea {
            id: badgeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: editor.sidechainMenuRequested(badge)
        }
        ToolTip.visible: badgeArea.containsMouse
        ToolTip.delay: 700
        ToolTip.text: qsTr("Sidechain: lit while another track's spectrum keys the gains. Click for the "
                           + "sidechain menu")
    }

    EditorDivider {
        x: graph.x + graph.width + editor.gap / 2
    }

    // Where it acts (the Focus band's edges, as values: typed, scrolled, automated and mapped as any control),
    // then time and output.
    Grid {
        id: right
        x: editor.width - 8 - editor.rightWidth
        y: 6
        columns: 4
        columnSpacing: 0
        rowSpacing: 8

        FocusCell {
            id: focusLow
            paramId: "focus_lo"
            title: qsTr("Focus Low")
            tooltip: qsTr("Focus Low: nothing below this is changed (it fades out over a third of an octave "
                          + "under it). Its edge in the display drags it too")
        }
        EditorKnob {
            id: attackKnob
            objectName: "attack"
            width: editor.cellWidth
            param: p.get("attack")
            title: qsTr("Attack")
            tooltip: qsTr("Attack: how fast a frequency's gain follows its level up")
        }
        EditorKnob {
            id: releaseKnob
            objectName: "release"
            width: editor.cellWidth
            param: p.get("release")
            title: qsTr("Release")
            tooltip: qsTr("Release: how fast a frequency's gain recovers as its level falls")
        }
        EditorKnob {
            id: linkKnob
            objectName: "link"
            width: editor.cellWidth
            param: p.get("link")
            title: qsTr("Stereo Link")
            tooltip: qsTr("Stereo Link: 100 both channels get the same gains (the louder one's); 0 each "
                          + "channel its own")
        }
        FocusCell {
            id: focusHigh
            paramId: "focus_hi"
            title: qsTr("Focus High")
            tooltip: qsTr("Focus High: nothing above this is changed (it fades out over a third of an octave "
                          + "over it). Its edge in the display drags it too")
        }
        EditorKnob {
            id: mixKnob
            objectName: "mix"
            width: editor.cellWidth
            param: p.get("mix")
            title: qsTr("Dry/Wet")
            tooltip: qsTr("Dry/Wet: the processed sound blended with the input (in time: the input is delayed as much)")
        }
        EditorKnob {
            id: outputKnob
            objectName: "output"
            width: editor.cellWidth
            param: p.get("output")
            title: qsTr("Output")
            knob.bipolar: true
            tooltip: qsTr("Output: the level after everything")
        }
        Item {
            width: editor.cellWidth
            height: mixKnob.height

            ParamButton {
                objectName: "delta"
                anchors.centerIn: parent
                width: Math.max(editor.cellWidth - 6, Math.ceil(implicitWidth))
                param: p.get("delta")
                text: qsTr("Delta")
                tooltip: qsTr("Delta: hear only what the device changes (what it takes away; what it adds comes "
                              + "out inverted), to set it by ear")
            }
        }
    }
}
