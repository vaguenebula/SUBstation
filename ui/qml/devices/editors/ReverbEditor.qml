import QtQuick
import QtQuick.Controls
import SUBstation

// The Reverb's editor, laid out as Ableton's, left to right:
// - Input: Lo Cut and Hi Cut over an X-Y pad of the band the reverb hears
//   (ReverbFilterPad: across for its centre, up and down for its width, over
//   the input's spectrum), its frequency and width in boxes under it.
// - Early reflections: Spin over its X-Y pad (ReverbSpinPad: across for the
//   rate, up and down for the amount, over the reflections drawn as particles
//   swinging round the stereo field), its amount and rate in boxes; Shape over
//   Predelay.
// - Global: Size and Stereo over Density and Smooth.
// - Diffusion network: the Lo and Hi shelves' switches and the high filter's
//   type over the decay per frequency (ReverbDecayGraph: Decay's handle and the
//   shelves', over the tail's spectrum, with the tail's meter), the shelves'
//   frequencies and gains in boxes under it; Decay over Freeze, Flat and Cut;
//   Diffusion over Scale; Chorus (its switch the Amount knob's title) over Rate.
// - Output: Reflect over Diffuse, and Dry/Wet.
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click gives
// its menu; controls a switch leaves unused are dimmed, and stay editable.
//
// Everything with text is as wide as the font makes it: the knobs' columns their
// widest caption or readout (the house's 52 px at least), the boxes their widest
// text (sample texts in the font's widest digits) and the automation dot, the
// lists their longest names and the arrow, the switches their text. The pads and
// the graph span the boxes under them (and the switches over them, and their own
// captions), and everything to their right follows.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias filterPad: filterPad
    readonly property alias spinPad: spinPad
    readonly property alias decayGraph: decayGraph

    // The body: the sections side by side, 8 px in from either side.
    implicitWidth: mixKnob.x + mixKnob.width + 8
    implicitHeight: 6 + Math.max(2 * shapeKnob.implicitHeight + 8, 20 + decayGraph.implicitHeight + 4 + 18) + 6

    // The second row of knobs (at the bottom), the canvases' height (20 px under the top for their
    // switches, 4 px over their boxes) and where the boxes are.
    readonly property real row2: height - 6 - shapeKnob.implicitHeight
    readonly property real canvasHeight: height - 54
    readonly property real boxY: height - 24
    readonly property real dim: 0.55
    // A box's text this far in from either side at its widest: where the automation dot (drawn 3.5 to 8.5 px in from
    // its left) ends, so the dot never reaches a value. (ParamBox's own width, its text and 16 px, puts the text
    // half a pixel into the dot.)
    readonly property real boxMargin: 8.5
    // A knob's column: as wide as the widest caption or readout a knob shows (the house's 52 px at least).
    readonly property real cell: Math.max(52, Math.ceil(knobTexts.implicitWidth))
    // Chorus's column: a knob's, or as wide as its switch (the Amount knob's title) needs; an even width, so the
    // dial (centred on a whole pixel) and the switch over it share their centre.
    readonly property real chorusWidth: 2 * Math.ceil(Math.max(cell, chorusButton.implicitWidth) / 2)
    // Decay over Freeze, Flat and Cut: a column as wide as the widest of them (Freeze, with its snowflake).
    readonly property real freezeX: decayGraph.x + decayGraph.width + 4
    readonly property real freezeWidth: Math.max(cell, ...[freezeButton, flatButton, cutButton].map(
        button => Math.ceil(button.implicitWidth)))

    // The switches, as they are now.
    readonly property bool loCutOn: isOn("lo_cut")
    readonly property bool hiCutOn: isOn("hi_cut")
    readonly property bool spinOn: isOn("spin")
    readonly property bool loShelfOn: isOn("lo_shelf")
    readonly property bool hiFilterOn: isOn("hi_filter")
    readonly property bool hiLowpass: isOn("hi_type")
    readonly property bool freezeOn: isOn("freeze")
    readonly property bool chorusOn: isOn("chorus")

    function isOn(id) {
        const param = p.get(id)
        return param ? param.value >= 0.5 : false
    }

    // A sample text: `pattern` with each "d" the font's widest digit (figures may be proportional), so what is
    // measured is as wide as the widest value of that form, whatever the font.
    function sample(pattern) {
        return pattern.replace(/d/g, boxFont.widestDigit)
    }
    // A box's sample text: the widest of the forms its values take (as `sample` makes them).
    function boxSample(patterns) {
        return patterns.map(sample).reduce((widest, text) => boxFont.advance(text) > boxFont.advance(widest)
                                                                 ? text : widest)
    }

    FontMetrics {
        id: boxFont

        // (Read in a binding that names the font, so it is worked out again once the font is set.)
        readonly property string widestDigit: {
            void font
            let widest = "0"
            for (const digit of "123456789") {
                if (boxFont.advanceWidth(digit) > boxFont.advanceWidth(widest))
                    widest = digit
            }
            return widest
        }

        // `text`'s advance (the font followed, as above).
        function advance(text) {
            void font
            return advanceWidth(text)
        }

        font: Theme.uiFont(8)  // (ParamBox's, and the knobs' captions' and readouts')
    }

    // The widest of `lines` as a Text lays them out in its font (a caption, a readout or a switch's text is as
    // wide as its advance and whatever of its last glyph reaches past it): hidden, a line each.
    component Widest: Text {
        property var lines: []
        visible: false
        textFormat: Text.PlainText
        text: lines.join("\n")
    }
    // What the knobs show at their widest: their captions, and each readout's widest form, every figure the
    // font's widest (Predelay's "ddd ms" and "0.dd ms", Size's "ddd.dd", Stereo's degrees, Decay's "dd.dd s" and
    // "1000 ms" (999.6 rounded), the percentages, Rate's "d.dd Hz", the levels' "-dd.d dB"). In the captions' and
    // readouts' font.
    Widest {
        id: knobTexts
        font: Theme.uiFont(8)
        lines: [shapeKnob, predelayKnob, sizeKnob, stereoKnob, decayKnob, diffusionKnob, scaleKnob,
                chorusAmountKnob, chorusRateKnob, reflectKnob, diffuseKnob, mixKnob]
            .map(knob => knob.title)
            .concat(["ddd ms", "d.d ms", "0.dd ms", "1000 ms", "ddd.dd", "ddd°", "dd.dd s", "100 %", "d.dd Hz",
                     "-dd.d dB"]
                        .map(pattern => editor.sample(pattern)))
    }

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["predelay", "lo_cut", "hi_cut", "in_freq", "in_width", "spin", "spin_rate", "spin_amount", "shape",
              "density", "smooth", "size", "stereo", "lo_shelf", "lo_freq", "lo_gain", "hi_filter", "hi_type",
              "hi_freq", "hi_gain", "decay", "freeze", "flat", "cut", "diffusion", "scale", "chorus", "chorus_rate",
              "chorus_amount", "reflect", "diffuse", "mix"]
    }

    // A knob in its column.
    component Knob: EditorKnob {
        width: editor.cell
    }
    // A value box for a parameter, as the Delay's: the parameter's own text, typed values parsed as it reads them;
    // as wide as its sample text (its widest) with `boxMargin` either side.
    component Box: ParamBox {
        formatter: v => param ? param.format(v) : ""
        parser: text => param ? param.parse(text) : null
        defaultValue: param ? param.defaultValue : 0
        width: Math.ceil(boxFont.advance(sampleText) + 2 * editor.boxMargin)  // (on whole pixels)
        height: 18
        y: editor.boxY
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    // A switch that fades with what it depends on.
    component FadingButton: ParamButton {
        height: 16
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    // A list's names in its font: the advance of the longest (a last glyph reaching past it stays clear of the
    // arrow, in the list's right padding).
    component Names: FontMetrics {
        function widest(names) {
            void font  // (worked out again once the font is set)
            let most = 0
            for (const name of names)
                most = Math.max(most, advanceWidth(name))
            return most
        }
    }
    // A list under its caption, the two in the middle of a row of knobs; as wide as its longest name and
    // the arrow, and its caption (and a knob's column at least).
    component Choice: Column {
        id: choice

        property alias param: list.param
        property alias title: caption.text
        property alias tooltip: list.tooltip
        property alias listName: list.objectName  // (the list's: tests find it by its name)
        readonly property alias list: list

        width: Math.max(editor.cell, Math.ceil(names.widest(list.names)) + list.button.leftPadding
                        + list.button.rightPadding, Math.ceil(caption.implicitWidth))
        spacing: 2

        Names {
            id: names
            font: list.button.font
        }
        EditorCaption {
            id: caption
            width: parent.width
        }
        ParamChoice {
            id: list
            width: parent.width
            height: 16
        }
    }

    // --- Input ---------------------------------------------------------------------------------

    FadingButton {
        id: loCutButton
        objectName: "loCutButton"
        x: filterPad.x
        y: 6
        width: Math.floor((filterPad.width - 4) / 2)
        param: p.get("lo_cut")
        text: qsTr("Lo Cut")
        tooltip: qsTr("Lo Cut: a high-pass on what goes into the reverb, at the band's low edge")
    }
    FadingButton {
        id: hiCutButton
        objectName: "hiCutButton"
        x: filterPad.x + filterPad.width - width
        y: 6
        width: Math.floor((filterPad.width - 4) / 2)
        param: p.get("hi_cut")
        text: qsTr("Hi Cut")
        tooltip: qsTr("Hi Cut: a low-pass on what goes into the reverb, at the band's high edge")
    }
    ReverbFilterPad {
        id: filterPad
        objectName: "filterPad"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 8
        y: 26
        // Its boxes side by side, or wider for the cuts' switches over it (each half of it) or its caption.
        width: Math.max(inFreqBox.width + 4 + inWidthBox.width,
                        2 * Math.max(Math.ceil(loCutButton.implicitWidth), Math.ceil(hiCutButton.implicitWidth)) + 4,
                        implicitWidth)
        height: editor.canvasHeight

        HoverHandler {
            id: filterHover
        }
        ToolTip.visible: filterHover.hovered && !filterHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("The band the reverb hears. Drag across for its centre, up and down for its width")
    }
    Box {
        id: inFreqBox
        objectName: "inFreqBox"
        x: 8
        opacity: editor.loCutOn || editor.hiCutOn ? 1 : editor.dim
        param: p.get("in_freq")
        logScale: true
        decimals: 0
        sampleText: editor.boxSample(["ddd Hz", "1000 Hz", "d.dd kHz", "1d.dd kHz"])  // (50 Hz to 18 kHz)
        tooltip: qsTr("In Filter Freq: the centre of the band the reverb hears")
    }
    Box {
        id: inWidthBox
        objectName: "inWidthBox"
        x: filterPad.x + filterPad.width - width  // (under the pad's right edge)
        opacity: editor.loCutOn || editor.hiCutOn ? 1 : editor.dim
        param: p.get("in_width")
        step: 0.05
        decimals: 2
        sampleText: editor.boxSample(["d.dd oct"])
        tooltip: qsTr("In Filter Width: how wide that band is, in octaves")
    }

    // --- Early reflections ---------------------------------------------------------------------

    FadingButton {
        id: spinButton
        objectName: "spinButton"
        x: spinPad.x
        y: 6
        width: spinPad.width
        param: p.get("spin")
        text: qsTr("Spin")
        tooltip: qsTr("Spin: the early reflections drift in time and swing around the stereo field "
                      + "(the tail hears them drift too)")
    }
    ReverbSpinPad {
        id: spinPad
        objectName: "spinPad"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: spinAmountBox.x
        y: 26
        // Its boxes side by side, or wider for Spin's switch or its captions ("Early", the tail's onset).
        width: Math.max(spinAmountBox.width + 4 + spinRateBox.width, Math.ceil(spinButton.implicitWidth),
                        implicitWidth)
        height: editor.canvasHeight

        HoverHandler {
            id: spinHover
        }
        ToolTip.visible: spinHover.hovered && !spinHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("Spin. Drag across for the rate, up and down for the amount")
    }
    Box {
        id: spinAmountBox
        objectName: "spinAmountBox"
        x: filterPad.x + filterPad.width + 8
        opacity: editor.spinOn ? 1 : editor.dim
        param: p.get("spin_amount")
        step: 0.5
        decimals: 1
        sampleText: editor.boxSample(["dd %", "100 %"])
        tooltip: qsTr("ER Spin Amount: how far the reflections drift")
    }
    Box {
        id: spinRateBox
        objectName: "spinRateBox"
        x: spinPad.x + spinPad.width - width
        opacity: editor.spinOn ? 1 : editor.dim
        param: p.get("spin_rate")
        logScale: true
        decimals: 2
        sampleText: editor.boxSample(["d.dd Hz"])
        tooltip: qsTr("ER Spin Rate: how fast they drift (fast: doppler pitch and swirling pans)")
    }
    Knob {
        id: shapeKnob
        objectName: "shapeKnob"
        x: spinPad.x + spinPad.width + 4
        y: 6
        param: p.get("shape")
        title: qsTr("Shape")
        tooltip: qsTr("Shape: low, the reflections fade slowly and the tail starts early; "
                      + "high, they fade fast and the tail starts later")
    }
    Knob {
        id: predelayKnob
        objectName: "predelayKnob"
        x: shapeKnob.x
        y: editor.row2
        param: p.get("predelay")
        title: qsTr("Predelay")
        tooltip: qsTr("Predelay: the time before the first reflection")
    }

    EditorDivider {
        id: inputEdge
        x: shapeKnob.x + shapeKnob.width + 4
    }

    // --- Global --------------------------------------------------------------------------------

    Knob {
        id: sizeKnob
        objectName: "sizeKnob"
        x: densityChoice.x + (densityChoice.width - width) / 2
        y: 6
        param: p.get("size")
        title: qsTr("Size")
        tooltip: qsTr("Size: the room's size (every delay of the reverb grows with it)")
    }
    Knob {
        id: stereoKnob
        objectName: "stereoKnob"
        x: smoothChoice.x + (smoothChoice.width - width) / 2
        y: 6
        param: p.get("stereo")
        title: qsTr("Stereo")
        tooltip: qsTr("Stereo: the reverb's width, from mono to two sides independent of each other (at 120°)")
    }
    Choice {
        id: densityChoice
        listName: "densityChoice"
        x: inputEdge.x + 4
        y: editor.row2 + (shapeKnob.implicitHeight - implicitHeight) / 2
        param: p.get("density")
        title: qsTr("Density")
        tooltip: qsTr("Density: how many lines make the tail "
                      + "(Sparse: grainy and light on the CPU; High: the richest)")
    }
    Choice {
        id: smoothChoice
        listName: "smoothChoice"
        x: densityChoice.x + densityChoice.width + 2
        y: editor.row2 + (shapeKnob.implicitHeight - implicitHeight) / 2
        param: p.get("smooth")
        title: qsTr("Smooth")
        tooltip: qsTr("Smooth: how a change of Size reaches the tail "
                      + "(None: at once, with a pitch sweep; Slow, Fast: gliding)")
    }

    EditorDivider {
        id: globalEdge
        x: smoothChoice.x + smoothChoice.width + 4
    }

    // --- Diffusion network ---------------------------------------------------------------------

    FadingButton {
        id: loShelfButton
        objectName: "loShelfButton"
        x: decayGraph.x
        y: 6
        width: Math.max(40, Math.ceil(implicitWidth))
        param: p.get("lo_shelf")
        text: qsTr("Lo")
        tooltip: qsTr("Lo Shelf: the lows die away faster (drag the handle in the graph)")
    }
    FadingButton {
        id: hiFilterButton
        objectName: "hiFilterButton"
        x: loShelfButton.x + loShelfButton.width + 4
        y: 6
        width: Math.max(40, Math.ceil(implicitWidth))
        param: p.get("hi_filter")
        text: qsTr("Hi")
        tooltip: qsTr("Hi Filter: the highs die away faster: a shelf, or a low-pass on every pass")
    }
    ParamChoice {
        id: hiTypeChoice
        objectName: "hiTypeChoice"

        // Its longest name with the arrow.
        readonly property real needed: Math.ceil(hiTypeNames.widest(names)) + button.leftPadding + button.rightPadding

        x: hiFilterButton.x + hiFilterButton.width + 4
        y: 6
        width: decayGraph.x + decayGraph.width - x
        height: 16
        opacity: editor.hiFilterOn ? 1 : editor.dim
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
        param: p.get("hi_type")
        tooltip: qsTr("Hi Filter Type: a shelf (the highs ring for a share of Decay) "
                      + "or a low-pass (darker with every pass)")

        Names {
            id: hiTypeNames
            font: hiTypeChoice.button.font
        }
    }
    ReverbDecayGraph {
        id: decayGraph
        objectName: "decayGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: loFreqBox.x
        y: 26
        // Its boxes side by side (the shelves' pairs 8 px apart), or wider for the switches and the type over it
        // or its captions ("Decay time", the widest readout).
        width: Math.max(loFreqBox.width + 4 + loGainBox.width + 8 + hiFreqBox.width + 4 + hiGainBox.width,
                        hiTypeChoice.x - x + hiTypeChoice.needed, implicitWidth)
        height: editor.canvasHeight

        HoverHandler {
            id: decayHover
        }
        ToolTip.visible: decayHover.hovered && !decayHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("How long each frequency rings. Drag the middle for Decay, the handles for the shelves "
                           + "(across: the frequency, up and down: how long that band rings); "
                           + "double-click a handle to switch it")
    }
    Box {
        id: loFreqBox
        objectName: "loFreqBox"
        x: globalEdge.x + 4
        opacity: editor.loShelfOn ? 1 : editor.dim
        param: p.get("lo_freq")
        logScale: true
        decimals: 0
        sampleText: editor.boxSample(["dd Hz", "ddd Hz", "1000 Hz", "d.dd kHz", "1d.dd kHz"])
        tooltip: qsTr("Lo Shelf Freq: where the lows start dying away faster")
    }
    Box {
        id: loGainBox
        objectName: "loGainBox"
        x: loFreqBox.x + loFreqBox.width + 4
        opacity: editor.loShelfOn ? 1 : editor.dim
        param: p.get("lo_gain")
        step: 1
        decimals: 0
        sampleText: editor.boxSample(["dd %", "100 %"])
        tooltip: qsTr("Lo Shelf Gain: how long the lows ring, as a share of Decay")
    }
    Box {
        id: hiFreqBox
        objectName: "hiFreqBox"
        x: hiGainBox.x - 4 - width
        opacity: editor.hiFilterOn ? 1 : editor.dim
        param: p.get("hi_freq")
        logScale: true
        decimals: 0
        sampleText: editor.boxSample(["dd Hz", "ddd Hz", "1000 Hz", "d.dd kHz", "1d.dd kHz"])
        tooltip: qsTr("Hi Filter Freq: where the highs start dying away faster")
    }
    Box {
        id: hiGainBox
        objectName: "hiGainBox"
        x: decayGraph.x + decayGraph.width - width  // (under the graph's right edge)
        opacity: editor.hiFilterOn && !editor.hiLowpass ? 1 : editor.dim
        param: p.get("hi_gain")
        step: 1
        decimals: 0
        sampleText: editor.boxSample(["dd %", "100 %"])
        tooltip: qsTr("Hi Shelf Gain: how long the highs ring, as a share of Decay (unused by the low-pass)")
    }

    Knob {
        id: decayKnob
        objectName: "decayKnob"
        x: editor.freezeX + (editor.freezeWidth - width) / 2
        y: 6
        param: p.get("decay")
        title: qsTr("Decay")
        tooltip: qsTr("Decay Time: how long the tail takes to fall 60 dB")
    }
    FadingButton {
        id: freezeButton
        objectName: "freezeButton"
        x: editor.freezeX
        y: editor.height - 62
        width: editor.freezeWidth
        param: p.get("freeze")
        text: qsTr("Freeze")
        iconName: "snowflake"
        tooltip: qsTr("Freeze: the tail holds for ever")
    }
    FadingButton {
        id: flatButton
        objectName: "flatButton"
        x: editor.freezeX
        y: editor.height - 42
        width: editor.freezeWidth
        opacity: editor.freezeOn ? 1 : editor.dim
        param: p.get("flat")
        text: qsTr("Flat")
        tooltip: qsTr("Flat: frozen, every band holds (off: the shelves still take their bands away)")
    }
    FadingButton {
        id: cutButton
        objectName: "cutButton"
        x: editor.freezeX
        y: editor.height - 22
        width: editor.freezeWidth
        opacity: editor.freezeOn ? 1 : editor.dim
        param: p.get("cut")
        text: qsTr("Cut")
        tooltip: qsTr("Cut: frozen, new sound no longer reaches the tail")
    }
    Knob {
        id: diffusionKnob
        objectName: "diffusionKnob"
        x: editor.freezeX + editor.freezeWidth + 2
        y: 6
        param: p.get("diffusion")
        title: qsTr("Diffusion")
        tooltip: qsTr("Diffusion: how quickly the echoes blur into a smooth tail")
    }
    Knob {
        id: scaleKnob
        objectName: "scaleKnob"
        x: diffusionKnob.x
        y: editor.row2
        param: p.get("scale")
        title: qsTr("Scale")
        tooltip: qsTr("Scale: how coarse that blur is (more noticeable in small rooms)")
    }

    EditorDivider {
        id: networkEdge
        x: diffusionKnob.x + diffusionKnob.width + 4
    }

    // Chorus: Amount in the first row with the other knobs, its switch the knob's title; Rate below. The switch
    // fills the strip of the knob's own caption (empty) down to 1 px over the dial, a little shorter than the
    // other switches. It comes after the knob, so it lies on top: the mouse there hovers the switch, not the knob.
    EditorKnob {
        id: chorusAmountKnob
        objectName: "chorusAmountKnob"
        x: networkEdge.x + 4
        y: 6
        width: editor.chorusWidth
        opacity: editor.chorusOn ? 1 : editor.dim
        param: p.get("chorus_amount")
        tooltip: qsTr("Chorus Amount: how far they drift")
    }
    FadingButton {
        id: chorusButton
        objectName: "chorusButton"
        x: chorusAmountKnob.x
        y: chorusAmountKnob.y
        width: editor.chorusWidth
        height: chorusAmountKnob.knob.y - 1
        param: p.get("chorus")
        text: qsTr("Chorus")
        tooltip: qsTr("Chorus: the tail's echoes drift in pitch, for a lusher, less metallic sound")
    }
    EditorKnob {
        id: chorusRateKnob
        objectName: "chorusRateKnob"
        x: chorusAmountKnob.x
        y: editor.row2
        width: editor.chorusWidth
        opacity: editor.chorusOn ? 1 : editor.dim
        param: p.get("chorus_rate")
        title: qsTr("Rate")
        tooltip: qsTr("Chorus Rate: how fast")
    }

    EditorDivider {
        id: chorusEdge
        x: chorusButton.x + chorusButton.width + 4
    }

    // --- Output --------------------------------------------------------------------------------

    Knob {
        id: reflectKnob
        objectName: "reflectKnob"
        x: chorusEdge.x + 4
        y: 6
        param: p.get("reflect")
        title: qsTr("Reflect")
        tooltip: qsTr("Reflect: the early reflections' level")
    }
    Knob {
        id: diffuseKnob
        objectName: "diffuseKnob"
        x: chorusEdge.x + 4
        y: editor.row2
        param: p.get("diffuse")
        title: qsTr("Diffuse")
        tooltip: qsTr("Diffuse: the tail's level")
    }
    Knob {
        id: mixKnob
        objectName: "mixKnob"
        x: reflectKnob.x + reflectKnob.width + 2
        y: 6
        param: p.get("mix")
        title: qsTr("Dry/Wet")
        tooltip: qsTr("Dry/Wet: the input blended with the reverb")
    }
}
