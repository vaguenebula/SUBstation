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
//   Diffusion over Scale; Chorus with its amount and rate.
// - Output: Reflect over Diffuse, and Dry/Wet.
// Every control shows its parameter as it is now (its automation's value while
// that plays), sets it undoably, touches it when pressed, and right-click gives
// its menu; controls a switch leaves unused are dimmed, and stay editable.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias filterPad: filterPad
    readonly property alias spinPad: spinPad
    readonly property alias decayGraph: decayGraph

    // The body: eight columns of 52 px knobs and the three canvases, 8 px in from either side.
    implicitWidth: 890
    implicitHeight: 6 + Math.max(2 * shapeKnob.implicitHeight + 8,
                                 20 + chorusAmountKnob.implicitHeight + 4 + chorusRateKnob.implicitHeight,
                                 20 + decayGraph.implicitHeight + 4 + 18) + 6

    // The second row of knobs (at the bottom), the canvases' height (20 px under the top for their
    // switches, 4 px over their boxes) and where the boxes are.
    readonly property real row2: height - 6 - shapeKnob.implicitHeight
    readonly property real canvasHeight: height - 54
    readonly property real boxY: height - 24
    readonly property real dim: 0.55

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

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["predelay", "lo_cut", "hi_cut", "in_freq", "in_width", "spin", "spin_rate", "spin_amount", "shape",
              "density", "smooth", "size", "stereo", "lo_shelf", "lo_freq", "lo_gain", "hi_filter", "hi_type",
              "hi_freq", "hi_gain", "decay", "freeze", "flat", "cut", "diffusion", "scale", "chorus", "chorus_rate",
              "chorus_amount", "reflect", "diffuse", "mix"]
    }

    // A value box for a parameter, as the Delay's: the parameter's own text, typed values parsed as it reads them.
    component Box: ParamBox {
        formatter: v => param ? param.format(v) : ""
        parser: text => param ? param.parse(text) : null
        defaultValue: param ? param.defaultValue : 0
        height: 18
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    // A switch that fades with what it depends on.
    component Switch: ParamButton {
        height: 16
        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
    // A list under its caption, the two in the middle of a row of knobs.
    component Choice: Column {
        id: choice

        property alias param: list.param
        property alias title: caption.text
        property alias tooltip: list.tooltip
        property alias listName: list.objectName  // (the list's: tests find it by its name)
        readonly property alias list: list

        width: 52
        spacing: 2

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
    // A section's edge.
    component Rule: Rectangle {
        y: 6
        width: 1
        height: editor.height - 12
        color: Theme.border
        opacity: 0.7
    }

    // --- Input ---------------------------------------------------------------------------------

    Switch {
        objectName: "loCutButton"
        x: 8
        y: 6
        width: 52
        param: p.get("lo_cut")
        text: qsTr("Lo Cut")
        tooltip: qsTr("Lo Cut: a high-pass on what goes into the reverb, at the band's low edge")
    }
    Switch {
        objectName: "hiCutButton"
        x: 64
        y: 6
        width: 52
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
        width: 108
        height: editor.canvasHeight

        HoverHandler {
            id: filterHover
        }
        ToolTip.visible: filterHover.hovered && !filterHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("The band the reverb hears. Drag across for its centre, up and down for its width")
    }
    Box {
        objectName: "inFreqBox"
        x: 8
        y: editor.boxY
        width: 52
        opacity: editor.loCutOn || editor.hiCutOn ? 1 : editor.dim
        param: p.get("in_freq")
        logScale: true
        decimals: 0
        sampleText: "18.0 kHz"
        tooltip: qsTr("In Filter Freq: the centre of the band the reverb hears")
    }
    Box {
        objectName: "inWidthBox"
        x: 64
        y: editor.boxY
        width: 52
        opacity: editor.loCutOn || editor.hiCutOn ? 1 : editor.dim
        param: p.get("in_width")
        step: 0.05
        decimals: 2
        sampleText: "7.50 oct"
        tooltip: qsTr("In Filter Width: how wide that band is, in octaves")
    }

    // --- Early reflections ---------------------------------------------------------------------

    Switch {
        objectName: "spinButton"
        x: 124
        y: 6
        width: 96
        param: p.get("spin")
        text: qsTr("Spin")
        tooltip: qsTr("Spin: the early reflections drift in time and swing around the stereo field")
    }
    ReverbSpinPad {
        id: spinPad
        objectName: "spinPad"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 124
        y: 26
        width: 96
        height: editor.canvasHeight

        HoverHandler {
            id: spinHover
        }
        ToolTip.visible: spinHover.hovered && !spinHover.point.pressedButtons
        ToolTip.delay: 700
        ToolTip.text: qsTr("Spin. Drag across for the rate, up and down for the amount")
    }
    Box {
        objectName: "spinAmountBox"
        x: 124
        y: editor.boxY
        width: 46
        opacity: editor.spinOn ? 1 : editor.dim
        param: p.get("spin_amount")
        step: 0.5
        decimals: 1
        sampleText: "100 %"
        tooltip: qsTr("ER Spin Amount: how far the reflections drift")
    }
    Box {
        objectName: "spinRateBox"
        x: 174
        y: editor.boxY
        width: 46
        opacity: editor.spinOn ? 1 : editor.dim
        param: p.get("spin_rate")
        logScale: true
        decimals: 2
        sampleText: "0.30 Hz"
        tooltip: qsTr("ER Spin Rate: how fast they drift (fast: doppler pitch and swirling pans)")
    }
    EditorKnob {
        id: shapeKnob
        objectName: "shapeKnob"
        x: 224
        y: 6
        param: p.get("shape")
        title: qsTr("Shape")
        tooltip: qsTr("Shape: low, the reflections fade slowly and the tail starts early; "
                      + "high, they fade fast and the tail starts later")
    }
    EditorKnob {
        objectName: "predelayKnob"
        x: 224
        y: editor.row2
        param: p.get("predelay")
        title: qsTr("Predelay")
        tooltip: qsTr("Predelay: the time before the first reflection")
    }

    Rule {
        x: 280
    }

    // --- Global --------------------------------------------------------------------------------

    EditorKnob {
        objectName: "sizeKnob"
        x: 284
        y: 6
        param: p.get("size")
        title: qsTr("Size")
        tooltip: qsTr("Size: the room's size (every delay of the reverb grows with it)")
    }
    EditorKnob {
        objectName: "stereoKnob"
        x: 338
        y: 6
        param: p.get("stereo")
        title: qsTr("Stereo")
        tooltip: qsTr("Stereo: the reverb's width, from mono to wider than natural")
    }
    Choice {
        listName: "densityChoice"
        x: 284
        y: editor.row2 + (shapeKnob.implicitHeight - implicitHeight) / 2
        param: p.get("density")
        title: qsTr("Density")
        tooltip: qsTr("Density: how many lines make the tail "
                      + "(Sparse: grainy and light on the CPU; High: the richest)")
    }
    Choice {
        listName: "smoothChoice"
        x: 338
        y: editor.row2 + (shapeKnob.implicitHeight - implicitHeight) / 2
        param: p.get("smooth")
        title: qsTr("Smooth")
        tooltip: qsTr("Smooth: how a change of Size reaches the tail "
                      + "(None: at once, with a pitch sweep; Slow, Fast: gliding)")
    }

    Rule {
        x: 394
    }

    // --- Diffusion network ---------------------------------------------------------------------

    Switch {
        objectName: "loShelfButton"
        x: 398
        y: 6
        width: 40
        param: p.get("lo_shelf")
        text: qsTr("Lo")
        tooltip: qsTr("Lo Shelf: the lows die away faster (drag the handle in the graph)")
    }
    Switch {
        objectName: "hiFilterButton"
        x: 442
        y: 6
        width: 40
        param: p.get("hi_filter")
        text: qsTr("Hi")
        tooltip: qsTr("Hi Filter: the highs die away faster: a shelf, or a low-pass on every pass")
    }
    ParamChoice {
        objectName: "hiTypeChoice"
        x: 486
        y: 6
        width: 112
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
        x: 398
        y: 26
        width: 200
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
        objectName: "loFreqBox"
        x: 398
        y: editor.boxY
        width: 52
        opacity: editor.loShelfOn ? 1 : editor.dim
        param: p.get("lo_freq")
        logScale: true
        decimals: 0
        sampleText: "15.0 kHz"
        tooltip: qsTr("Lo Shelf Freq: where the lows start dying away faster")
    }
    Box {
        objectName: "loGainBox"
        x: 454
        y: editor.boxY
        width: 40
        opacity: editor.loShelfOn ? 1 : editor.dim
        param: p.get("lo_gain")
        step: 1
        decimals: 0
        sampleText: "100 %"
        tooltip: qsTr("Lo Shelf Gain: how long the lows ring, as a share of Decay")
    }
    Box {
        objectName: "hiFreqBox"
        x: 502
        y: editor.boxY
        width: 52
        opacity: editor.hiFilterOn ? 1 : editor.dim
        param: p.get("hi_freq")
        logScale: true
        decimals: 0
        sampleText: "16.0 kHz"
        tooltip: qsTr("Hi Filter Freq: where the highs start dying away faster")
    }
    Box {
        objectName: "hiGainBox"
        x: 558
        y: editor.boxY
        width: 40
        opacity: editor.hiFilterOn && !editor.hiLowpass ? 1 : editor.dim
        param: p.get("hi_gain")
        step: 1
        decimals: 0
        sampleText: "100 %"
        tooltip: qsTr("Hi Shelf Gain: how long the highs ring, as a share of Decay (unused by the low-pass)")
    }
    EditorKnob {
        objectName: "decayKnob"
        x: 602
        y: 6
        param: p.get("decay")
        title: qsTr("Decay")
        tooltip: qsTr("Decay Time: how long the tail takes to fall 60 dB")
    }
    Switch {
        objectName: "freezeButton"
        x: 602
        y: editor.height - 62
        width: 52
        param: p.get("freeze")
        text: qsTr("Freeze")
        iconName: "snowflake"
        tooltip: qsTr("Freeze: the tail holds for ever")
    }
    Switch {
        objectName: "flatButton"
        x: 602
        y: editor.height - 42
        width: 52
        opacity: editor.freezeOn ? 1 : editor.dim
        param: p.get("flat")
        text: qsTr("Flat")
        tooltip: qsTr("Flat: frozen, every band holds (off: the shelves still take their bands away)")
    }
    Switch {
        objectName: "cutButton"
        x: 602
        y: editor.height - 22
        width: 52
        opacity: editor.freezeOn ? 1 : editor.dim
        param: p.get("cut")
        text: qsTr("Cut")
        tooltip: qsTr("Cut: frozen, new sound no longer reaches the tail")
    }
    EditorKnob {
        objectName: "diffusionKnob"
        x: 656
        y: 6
        param: p.get("diffusion")
        title: qsTr("Diffusion")
        tooltip: qsTr("Diffusion: how quickly the echoes blur into a smooth tail")
    }
    EditorKnob {
        objectName: "scaleKnob"
        x: 656
        y: editor.row2
        param: p.get("scale")
        title: qsTr("Scale")
        tooltip: qsTr("Scale: how coarse that blur is (more noticeable in small rooms)")
    }

    Rule {
        x: 712
    }

    Switch {
        objectName: "chorusButton"
        x: 716
        y: 6
        width: 52
        param: p.get("chorus")
        text: qsTr("Chorus")
        tooltip: qsTr("Chorus: the tail's echoes drift in pitch, for a lusher, less metallic sound")
    }
    EditorKnob {
        id: chorusAmountKnob
        objectName: "chorusAmountKnob"
        x: 716
        y: 26
        size: 26
        opacity: editor.chorusOn ? 1 : editor.dim
        param: p.get("chorus_amount")
        title: qsTr("Amount")
        tooltip: qsTr("Chorus Amount: how far they drift")
    }
    EditorKnob {
        id: chorusRateKnob
        objectName: "chorusRateKnob"
        x: 716
        y: editor.height - 6 - height
        size: 26
        opacity: editor.chorusOn ? 1 : editor.dim
        param: p.get("chorus_rate")
        title: qsTr("Rate")
        tooltip: qsTr("Chorus Rate: how fast")
    }

    Rule {
        x: 772
    }

    // --- Output --------------------------------------------------------------------------------

    EditorKnob {
        objectName: "reflectKnob"
        x: 776
        y: 6
        param: p.get("reflect")
        title: qsTr("Reflect")
        tooltip: qsTr("Reflect: the early reflections' level")
    }
    EditorKnob {
        objectName: "diffuseKnob"
        x: 776
        y: editor.row2
        param: p.get("diffuse")
        title: qsTr("Diffuse")
        tooltip: qsTr("Diffuse: the tail's level")
    }
    EditorKnob {
        objectName: "mixKnob"
        x: 830
        y: 6
        param: p.get("mix")
        title: qsTr("Dry/Wet")
        tooltip: qsTr("Dry/Wet: the input blended with the reverb")
    }
}
