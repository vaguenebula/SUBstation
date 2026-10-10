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
// The boxes and lists are as wide as their widest text (the boxes' sample texts
// in the font's widest digits, the lists' longest names); the pads and the graph
// span the boxes under them, and everything to their right follows.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias filterPad: filterPad
    readonly property alias spinPad: spinPad
    readonly property alias decayGraph: decayGraph

    // The body: the sections side by side, 8 px in from either side.
    implicitWidth: mixKnob.x + mixKnob.width + 8
    implicitHeight: 6 + Math.max(2 * shapeKnob.implicitHeight + 8,
                                 20 + decayGraph.implicitHeight + 4 + 18,
                                 18 + chorusAmountKnob.implicitHeight - chorusAmountKnob.knob.y + 4
                                 + shapeKnob.implicitHeight) + 6

    // The second row of knobs (at the bottom), the canvases' height (20 px under the top for their
    // switches, 4 px over their boxes) and where the boxes are.
    readonly property real row2: height - 6 - shapeKnob.implicitHeight
    readonly property real canvasHeight: height - 54
    readonly property real boxY: height - 24
    readonly property real dim: 0.55
    readonly property real cell: 52  // a knob's column
    // Decay over Freeze, Flat and Cut: a column as wide as Freeze (its snowflake and its name).
    readonly property real freezeX: decayGraph.x + decayGraph.width + 4
    readonly property real freezeWidth: Math.max(cell, Math.ceil(freezeButton.implicitWidth))

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

    // A box's sample text: `pattern` with each "d" the font's widest digit (figures may be proportional), so
    // the box is as wide as the widest value it shows, whatever the font.
    function sample(pattern) {
        return pattern.replace(/d/g, boxFont.widestDigit)
    }

    FontMetrics {
        id: boxFont

        readonly property string widestDigit: {
            let widest = "0"
            for (const digit of "123456789") {
                if (boxFont.advanceWidth(digit) > boxFont.advanceWidth(widest))
                    widest = digit
            }
            return widest
        }

        font: Theme.uiFont(8)  // (ParamBox's)
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

    // A value box for a parameter, as the Delay's: the parameter's own text, typed values parsed as it reads them;
    // as wide as its sample text (its widest) and the automation dot.
    component Box: ParamBox {
        formatter: v => param ? param.format(v) : ""
        parser: text => param ? param.parse(text) : null
        defaultValue: param ? param.defaultValue : 0
        width: Math.ceil(implicitWidth)
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
    // A list under its caption, the two in the middle of a row of knobs; as wide as its longest name and
    // the arrow (and a knob's column at least).
    component Choice: Column {
        id: choice

        property alias param: list.param
        property alias title: caption.text
        property alias tooltip: list.tooltip
        property alias listName: list.objectName  // (the list's: tests find it by its name)
        readonly property alias list: list

        width: Math.max(editor.cell, Math.ceil(metrics.widest(list.names)) + list.button.leftPadding
                        + list.button.rightPadding)
        spacing: 2

        FontMetrics {
            id: metrics

            function widest(names) {
                let most = 0
                for (const name of names)
                    most = Math.max(most, advanceWidth(name))
                return most
            }

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
        objectName: "loCutButton"
        x: filterPad.x
        y: 6
        width: Math.floor((filterPad.width - 4) / 2)
        param: p.get("lo_cut")
        text: qsTr("Lo Cut")
        tooltip: qsTr("Lo Cut: a high-pass on what goes into the reverb, at the band's low edge")
    }
    FadingButton {
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
        width: inWidthBox.x + inWidthBox.width - x
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
        sampleText: editor.sample("1d.dd kHz")  // (10 to 18 kHz)
        tooltip: qsTr("In Filter Freq: the centre of the band the reverb hears")
    }
    Box {
        id: inWidthBox
        objectName: "inWidthBox"
        x: inFreqBox.x + inFreqBox.width + 4
        opacity: editor.loCutOn || editor.hiCutOn ? 1 : editor.dim
        param: p.get("in_width")
        step: 0.05
        decimals: 2
        sampleText: editor.sample("d.dd oct")
        tooltip: qsTr("In Filter Width: how wide that band is, in octaves")
    }

    // --- Early reflections ---------------------------------------------------------------------

    FadingButton {
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
        width: spinRateBox.x + spinRateBox.width - x
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
        sampleText: "100 %"
        tooltip: qsTr("ER Spin Amount: how far the reflections drift")
    }
    Box {
        id: spinRateBox
        objectName: "spinRateBox"
        x: spinAmountBox.x + spinAmountBox.width + 4
        opacity: editor.spinOn ? 1 : editor.dim
        param: p.get("spin_rate")
        logScale: true
        decimals: 2
        sampleText: editor.sample("d.dd Hz")
        tooltip: qsTr("ER Spin Rate: how fast they drift (fast: doppler pitch and swirling pans)")
    }
    EditorKnob {
        id: shapeKnob
        objectName: "shapeKnob"
        x: spinPad.x + spinPad.width + 4
        y: 6
        param: p.get("shape")
        title: qsTr("Shape")
        tooltip: qsTr("Shape: low, the reflections fade slowly and the tail starts early; "
                      + "high, they fade fast and the tail starts later")
    }
    EditorKnob {
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

    EditorKnob {
        objectName: "sizeKnob"
        x: densityChoice.x + (densityChoice.width - width) / 2
        y: 6
        param: p.get("size")
        title: qsTr("Size")
        tooltip: qsTr("Size: the room's size (every delay of the reverb grows with it)")
    }
    EditorKnob {
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
        objectName: "loShelfButton"
        x: decayGraph.x
        y: 6
        width: 40
        param: p.get("lo_shelf")
        text: qsTr("Lo")
        tooltip: qsTr("Lo Shelf: the lows die away faster (drag the handle in the graph)")
    }
    FadingButton {
        objectName: "hiFilterButton"
        x: decayGraph.x + 44
        y: 6
        width: 40
        param: p.get("hi_filter")
        text: qsTr("Hi")
        tooltip: qsTr("Hi Filter: the highs die away faster: a shelf, or a low-pass on every pass")
    }
    ParamChoice {
        objectName: "hiTypeChoice"
        x: decayGraph.x + 88
        y: 6
        width: decayGraph.width - 88
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
    }
    ReverbDecayGraph {
        id: decayGraph
        objectName: "decayGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: loFreqBox.x
        y: 26
        width: hiGainBox.x + hiGainBox.width - x
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
        sampleText: editor.sample("1d.dd kHz")
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
        sampleText: "100 %"
        tooltip: qsTr("Lo Shelf Gain: how long the lows ring, as a share of Decay")
    }
    Box {
        id: hiFreqBox
        objectName: "hiFreqBox"
        x: loGainBox.x + loGainBox.width + 8
        opacity: editor.hiFilterOn ? 1 : editor.dim
        param: p.get("hi_freq")
        logScale: true
        decimals: 0
        sampleText: editor.sample("1d.dd kHz")
        tooltip: qsTr("Hi Filter Freq: where the highs start dying away faster")
    }
    Box {
        id: hiGainBox
        objectName: "hiGainBox"
        x: hiFreqBox.x + hiFreqBox.width + 4
        opacity: editor.hiFilterOn && !editor.hiLowpass ? 1 : editor.dim
        param: p.get("hi_gain")
        step: 1
        decimals: 0
        sampleText: "100 %"
        tooltip: qsTr("Hi Shelf Gain: how long the highs ring, as a share of Decay (unused by the low-pass)")
    }

    EditorKnob {
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
        objectName: "cutButton"
        x: editor.freezeX
        y: editor.height - 22
        width: editor.freezeWidth
        opacity: editor.freezeOn ? 1 : editor.dim
        param: p.get("cut")
        text: qsTr("Cut")
        tooltip: qsTr("Cut: frozen, new sound no longer reaches the tail")
    }
    EditorKnob {
        id: diffusionKnob
        objectName: "diffusionKnob"
        x: editor.freezeX + editor.freezeWidth + 2
        y: 6
        param: p.get("diffusion")
        title: qsTr("Diffusion")
        tooltip: qsTr("Diffusion: how quickly the echoes blur into a smooth tail")
    }
    EditorKnob {
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

    // Chorus: its switch is the Amount knob's title (the knob's own caption, empty, lies under it), Rate below.
    FadingButton {
        id: chorusButton
        objectName: "chorusButton"
        x: networkEdge.x + 4
        y: 6
        width: editor.cell
        param: p.get("chorus")
        text: qsTr("Chorus")
        tooltip: qsTr("Chorus: the tail's echoes drift in pitch, for a lusher, less metallic sound")
    }
    EditorKnob {
        id: chorusAmountKnob
        objectName: "chorusAmountKnob"
        x: chorusButton.x
        y: chorusButton.y + chorusButton.height + 2 - knob.y
        opacity: editor.chorusOn ? 1 : editor.dim
        param: p.get("chorus_amount")
        tooltip: qsTr("Chorus Amount: how far they drift")
    }
    EditorKnob {
        objectName: "chorusRateKnob"
        x: chorusButton.x
        y: editor.row2
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

    EditorKnob {
        objectName: "reflectKnob"
        x: chorusEdge.x + 4
        y: 6
        param: p.get("reflect")
        title: qsTr("Reflect")
        tooltip: qsTr("Reflect: the early reflections' level")
    }
    EditorKnob {
        objectName: "diffuseKnob"
        x: chorusEdge.x + 4
        y: editor.row2
        param: p.get("diffuse")
        title: qsTr("Diffuse")
        tooltip: qsTr("Diffuse: the tail's level")
    }
    EditorKnob {
        id: mixKnob
        objectName: "mixKnob"
        x: chorusEdge.x + 4 + editor.cell + 2
        y: 6
        param: p.get("mix")
        title: qsTr("Dry/Wet")
        tooltip: qsTr("Dry/Wet: the input blended with the reverb")
    }
}
