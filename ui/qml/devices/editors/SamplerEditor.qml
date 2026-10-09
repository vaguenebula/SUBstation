import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import SUBstation

// The Sampler's editor, laid out as Ableton's Simpler: two pages, named in the
// device's title bar.
//
// Sample: the mode tabs (Classic, 1-Shot, Slice) beside the sample's display
// (SampleView: its waveform, markers, loop, fades or slices) and, under it on
// the display, Gain, the mode's own controls (Classic: Loop and its fade;
// 1-Shot: Trigger or Gate; Slice: how it slices, how slices play, Trigger or
// Gate), Snap, and Warp (as how many beats, how, halved or doubled). Under the
// display: the filter (on, its shape, 12 or 24 dB, frequency, resonance), the
// LFO (on, Hz or synced, its shape, its rate), the envelope (Classic's ADSR,
// the others' fades), Transpose, Vol < Vel and Volume.
//
// Controls: what doesn't fit there: the root key, detune, voices and glide;
// Start, End and the loop as knobs, Reverse; where the LFO goes (volume,
// pitch, filter, pan) and whether notes restart it; pan.
//
// Drop an audio file on the display, or double-click it, to load one; drag its
// markers. The device's menu starts with Load Sample…, Clear Sample and
// Reverse (menuActions). Every control shows its parameter as it is now (its
// automation's value while that plays), sets it undoably, touches it when
// pressed, and right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property int pages: 2
    property int page: 0
    readonly property var pageNames: [qsTr("Sample"), qsTr("Controls")]
    // What the device's right-click menu starts with.
    readonly property list<Action> menuActions: [
        Action {
            objectName: "loadSample"
            text: qsTr("Load Sample…")
            onTriggered: editor.browse()
        },
        Action {
            objectName: "clearSample"
            text: qsTr("Clear Sample")
            enabled: view.samplePath !== ""
            onTriggered: view.clearSample()
        },
        Action {
            objectName: "reverseSample"
            text: qsTr("Reverse")
            checkable: true
            checked: editor.on("reverse")
            onTriggered: {
                editor.params["reverse"].set(editor.on("reverse") ? 0 : 1)
                checked = Qt.binding(() => editor.on("reverse"))
            }
        }
    ]
    readonly property alias view: view
    readonly property int mode: params["mode"] ? params["mode"].index : 0  // 0 Classic, 1 1-Shot, 2 Slice

    readonly property int margin: 8
    readonly property int modeWidth: 44
    readonly property int stripHeight: 18
    readonly property int minimumViewHeight: 40  // (SampleView's kMinimumHeight)
    readonly property int cellWidth: 50

    function browse() {
        if (view.sampleFolder.toString() !== "")
            fileDialog.currentFolder = view.sampleFolder
        fileDialog.open()
    }
    // A value box's text, the parameter's own (made again when the parameter comes: a box whose
    // value doesn't change would otherwise keep the text it had without one).
    function formatOf(param) {
        return param ? (v => param.format(v)) : null
    }
    // Whether an on/off parameter is on.
    function on(id) {
        return params[id] ? params[id].value >= 0.5 : false
    }

    // The device's body: Ableton's Simpler is about this wide.
    implicitWidth: 760
    implicitHeight: 6 + minimumViewHeight + stripHeight + 5 + bottomRow.implicitHeight + 6

    // The parameters, by id.
    readonly property var params: {
        const all = {}
        for (let i = 0; i < paramObjects.count; ++i) {
            const p = paramObjects.objectAt(i)
            if (p)
                all[p.paramId] = p
        }
        return all
    }
    Instantiator {
        id: paramObjects
        model: ["mode", "root", "tune", "fine", "start", "end", "gain", "reverse", "snap", "warp", "warp_beats",
                "warp_mode", "loop", "loop_start", "loop_fade", "attack", "decay", "sustain", "release", "voices",
                "glide", "trigger", "fade_in", "fade_out", "slice_by", "sensitivity", "slice_beat", "regions",
                "playback", "filter", "filter_type", "filter_slope", "filter_freq", "filter_res", "lfo", "lfo_wave",
                "lfo_sync", "lfo_rate", "lfo_beats", "lfo_retrig", "lfo_volume", "lfo_pitch", "lfo_filter",
                "lfo_pan", "pan", "velocity", "volume"]
        DeviceParam {
            required property string modelData
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            paramId: modelData
        }
    }

    component Caption: Text {
        horizontalAlignment: Text.AlignHCenter
        color: Theme.textDim
        font: Theme.uiFont(8)
        elide: Text.ElideRight
    }

    component Readout: Text {
        horizontalAlignment: Text.AlignHCenter
        color: enabled ? Theme.text : Theme.textDisabled
        font: Theme.uiFont(8)
        elide: Text.ElideRight
    }

    // A parameter's knob: its name over it, its value under it.
    component Cell: Column {
        id: cell

        property string paramId
        property string title: ""
        property bool wide: paramId === "filter_freq"
        readonly property DeviceParam param: editor.params[paramId] || null

        objectName: "cell_" + paramId
        width: wide ? 60 : editor.cellWidth
        spacing: 1

        Caption {
            width: parent.width
            text: cell.title !== "" ? cell.title : (cell.param ? cell.param.name : "")
        }
        ParamKnob {
            objectName: "knob_" + cell.paramId
            anchors.horizontalCenter: parent.horizontalCenter
            size: 22
            param: cell.param
            bipolar: cell.param ? cell.param.bipolar : false
            step: cell.param && cell.param.steps > 0 ? 1 : 0
        }
        Readout {
            width: parent.width
            text: cell.param ? cell.param.text : ""
        }
    }

    // A section of the Controls page: its title over its cells.
    component Section: Column {
        property string title
        spacing: 3

        Text {
            text: parent.title
            color: Theme.textDim
            font: Theme.uiFont(8, true)
        }
    }

    // One of the mode tabs: its icon over its name, lit while it is the mode.
    component ModeTab: Item {
        id: tab

        property int choice
        property string label
        property string iconName

        ParamArea {
            anchors.fill: parent
            param: editor.params["mode"] || null
        }
        RoleButton {
            id: tabButton
            objectName: "mode" + tab.choice
            anchors.fill: parent
            role: "small"
            checkable: false
            checked: editor.mode === tab.choice
            tooltip: [qsTr("Classic: played across the keyboard, looping if you like"),
                      qsTr("1-Shot: one note at a time, the whole sample (Trigger) or while held (Gate)"),
                      qsTr("Slice: cut at its transients, beats or into regions, a slice per key from C1")][tab.choice]
            onPressed: if (editor.params["mode"]) editor.params["mode"].touch()
            onClicked: if (editor.params["mode"]) editor.params["mode"].set(tab.choice)
            contentItem: Item {
                Column {
                    anchors.centerIn: parent
                    spacing: 1

                    Icon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        name: tab.iconName
                        size: 12
                        color: tabButton.look.text
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: tab.label
                        color: tabButton.look.text
                        font: Theme.uiFont(7.5)
                    }
                }
            }
        }
    }

    // --- Sample ----------------------------------------------------------------------------

    Item {
        id: samplePage
        objectName: "samplePage"
        anchors.fill: parent
        visible: editor.page === 0

        readonly property real panelHeight: Math.max(editor.minimumViewHeight + editor.stripHeight,
                                                     height - 12 - 5 - bottomRow.implicitHeight)

        Column {
            id: modeTabs
            x: editor.margin
            y: 6
            width: editor.modeWidth
            spacing: 2

            Repeater {
                model: [[qsTr("Classic"), "sampler_classic"], [qsTr("1-Shot"), "sampler_oneshot"],
                        [qsTr("Slice"), "sampler_slice"]]
                ModeTab {
                    required property var modelData
                    required property int index
                    width: modeTabs.width
                    height: (samplePage.panelHeight - 4) / 3
                    choice: index
                    label: modelData[0]
                    iconName: modelData[1]
                }
            }
        }

        // The display: the waveform, then a strip of the sample's settings on it.
        Rectangle {
            id: panel
            x: modeTabs.x + modeTabs.width + 4
            y: 6
            width: editor.width - x - editor.margin
            height: samplePage.panelHeight
            color: Theme.meterBg

            SampleView {
                id: view
                objectName: "sampleView"
                session: Session
                trackId: editor.trackId
                deviceId: editor.deviceId
                width: parent.width
                height: parent.height - editor.stripHeight
                onBrowseRequested: editor.browse()
            }

            Item {
                id: strip
                objectName: "strip"
                y: view.height
                width: parent.width
                height: editor.stripHeight

                Row {
                    id: stripLeft
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Caption {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Gain")
                    }
                    ParamBox {
                        objectName: "gain"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 60
                        height: 16
                        param: editor.params["gain"] || null
                        step: 0.1
                        decimals: 1
                        defaultValue: 0.0
                        formatter: editor.formatOf(param)
                        sampleText: "-24.0 dB"
                        tooltip: qsTr("The sample's gain")
                    }
                    Item {
                        width: 4
                        height: 1
                    }

                    // Classic: the loop, and how much of it fades.
                    ParamButton {
                        objectName: "loop"
                        visible: editor.mode === 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: 38
                        param: editor.params["loop"] || null
                        text: qsTr("Loop")
                        tooltip: qsTr("Loop from Loop Start to End while the note holds (drag its marker)")
                    }
                    Caption {
                        visible: editor.mode === 0 && editor.on("loop")
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Fade")
                    }
                    ParamBox {
                        objectName: "loopFade"
                        visible: editor.mode === 0 && editor.on("loop")
                        anchors.verticalCenter: parent.verticalCenter
                        width: 46
                        height: 16
                        param: editor.params["loop_fade"] || null
                        step: 0.5
                        decimals: 0
                        defaultValue: 0.0
                        formatter: editor.formatOf(param)
                        sampleText: "100 %"
                        tooltip: qsTr("Crossfade the loop's end into what leads to its start")
                    }

                    // Slice: how the sample is cut, and how slices play.
                    ParamChoice {
                        objectName: "sliceBy"
                        visible: editor.mode === 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 74
                        param: editor.params["slice_by"] || null
                        tooltip: qsTr("Slice at transients, at beats or into equal regions")
                    }
                    ParamBox {
                        objectName: "sensitivity"
                        visible: editor.mode === 2 && editor.params["slice_by"] && editor.params["slice_by"].index === 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: 42
                        height: 16
                        param: editor.params["sensitivity"] || null
                        step: 0.5
                        decimals: 0
                        defaultValue: 50.0
                        formatter: editor.formatOf(param)
                        sampleText: "100 %"
                        tooltip: qsTr("Sensitivity: more finds quieter transients")
                    }
                    ParamChoice {
                        objectName: "sliceBeat"
                        visible: editor.mode === 2 && editor.params["slice_by"] && editor.params["slice_by"].index === 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: 54
                        param: editor.params["slice_beat"] || null
                        tooltip: qsTr("A slice every this long (the sample is as many beats as Warp says)")
                    }
                    ParamBox {
                        objectName: "regions"
                        visible: editor.mode === 2 && editor.params["slice_by"] && editor.params["slice_by"].index === 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 34
                        height: 16
                        param: editor.params["regions"] || null
                        step: 1
                        decimals: 0
                        defaultValue: 8
                        formatter: editor.formatOf(param)
                        sampleText: "64"
                        tooltip: qsTr("How many equal slices")
                    }
                    ParamChoice {
                        objectName: "playback"
                        visible: editor.mode === 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 52
                        param: editor.params["playback"] || null
                        tooltip: qsTr("Mono: a slice cuts the one before; Poly: they overlap; Thru: a slice plays on to End")
                    }

                    // 1-Shot and Slice: the whole sample or slice, or while the note holds.
                    Row {
                        visible: editor.mode !== 0
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1

                        ParamButton {
                            objectName: "trigger"
                            width: 46
                            param: editor.params["trigger"] || null
                            choice: 0
                            text: qsTr("Trigger")
                            tooltip: qsTr("Trigger: it plays to its end, however short the note")
                        }
                        ParamButton {
                            objectName: "gate"
                            width: 36
                            param: editor.params["trigger"] || null
                            choice: 1
                            text: qsTr("Gate")
                            tooltip: qsTr("Gate: it fades out when the note ends")
                        }
                    }
                    ParamButton {
                        objectName: "snap"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 38
                        param: editor.params["snap"] || null
                        text: qsTr("Snap")
                        tooltip: qsTr("Snap: Start, End, Loop Start and slices start at zero crossings, without clicks")
                    }
                }

                // Warp: the whole sample in so many beats at the song's tempo.
                Row {
                    id: stripRight
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    ParamButton {
                        objectName: "warp"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 42
                        param: editor.params["warp"] || null
                        text: qsTr("Warp")
                        tooltip: qsTr("Warp: the sample plays in time with the song, as long as the beats beside")
                    }
                    Caption {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("as")
                    }
                    ParamBox {
                        objectName: "warpBeats"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 58
                        height: 16
                        param: editor.params["warp_beats"] || null
                        step: 1
                        decimals: 0
                        defaultValue: 4
                        formatter: editor.formatOf(param)
                        sampleText: "64 Bars"
                        tooltip: qsTr("How long the whole sample is, in beats")
                    }
                    ParamChoice {
                        objectName: "warpMode"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 76
                        param: editor.params["warp_mode"] || null
                        tooltip: qsTr("How it warps: stretched (the keys transpose it), or Re-Pitch (resampled, as a record)")
                    }
                    Repeater {
                        model: [[":2", 0.5, qsTr("Half as long")], ["*2", 2, qsTr("Twice as long")]]
                        RoleButton {
                            required property var modelData
                            objectName: modelData[1] < 1 ? "halve" : "double"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            height: 16
                            role: "small"
                            text: modelData[0]
                            tooltip: modelData[2]
                            onPressed: if (editor.params["warp_beats"]) editor.params["warp_beats"].touch()
                            onClicked: {
                                const beats = editor.params["warp_beats"]
                                if (beats)
                                    beats.set(Math.max(beats.minimum, Math.min(beats.maximum,
                                                                                Math.round(beats.value * modelData[1]))))
                            }
                        }
                    }
                }
            }
        }

        // Under the display: the filter, the LFO, the envelope, pitch and level.
        Row {
            id: bottomRow
            objectName: "bottomRow"
            x: editor.margin
            y: panel.y + panel.height + 5
            spacing: 8

            // The filter: on, its shape and slope; its frequency and resonance.
            Column {
                width: 52
                spacing: 1

                ParamButton {
                    objectName: "filter"
                    width: parent.width
                    param: editor.params["filter"] || null
                    text: qsTr("Filter")
                    tooltip: qsTr("The filter on what the notes play")
                }
                ParamChoice {
                    objectName: "filterType"
                    width: parent.width
                    param: editor.params["filter_type"] || null
                    icons: ["filter_lowpass", "filter_highpass", "filter_bandpass", "filter_notch"]
                    iconOnly: true
                    tooltip: editor.params["filter_type"] ? editor.params["filter_type"].text : ""
                }
                Row {
                    spacing: 1
                    Repeater {
                        model: ["12", "24"]
                        ParamButton {
                            required property string modelData
                            required property int index
                            objectName: "slope" + modelData
                            width: 25
                            param: editor.params["filter_slope"] || null
                            choice: index
                            text: modelData
                            tooltip: qsTr("%1 dB an octave").arg(modelData)
                        }
                    }
                }
            }
            Row {
                spacing: 0
                opacity: editor.on("filter") ? 1.0 : 0.55

                Cell {
                    paramId: "filter_freq"
                    title: qsTr("Frequency")
                }
                Cell {
                    paramId: "filter_res"
                    title: qsTr("Res")
                }
            }

            // The LFO: on, Hz or synced; its shape and rate.
            Column {
                width: 76
                spacing: 1

                Row {
                    spacing: 2
                    ParamButton {
                        objectName: "lfo"
                        width: 36
                        param: editor.params["lfo"] || null
                        text: qsTr("LFO")
                        tooltip: qsTr("The LFO (where it goes: the Controls page)")
                    }
                    ParamButton {
                        objectName: "lfoHz"
                        width: 18
                        param: editor.params["lfo_sync"] || null
                        choice: 0
                        text: qsTr("Hz")
                        tooltip: qsTr("Its rate in Hz")
                    }
                    ParamButton {
                        objectName: "lfoSynced"
                        width: 18
                        param: editor.params["lfo_sync"] || null
                        choice: 1
                        iconName: "note"
                        iconSize: 10
                        tooltip: qsTr("Its rate synced to the song")
                    }
                }
                ParamChoice {
                    objectName: "lfoWave"
                    width: parent.width
                    param: editor.params["lfo_wave"] || null
                    icons: ["wave_sine", "wave_triangle", "wave_saw_up", "wave_saw_down", "wave_square", "wave_random"]
                }
                Item {
                    width: parent.width
                    height: 16

                    ParamBox {
                        objectName: "lfoRate"
                        visible: !editor.on("lfo_sync")
                        anchors.fill: parent
                        param: editor.params["lfo_rate"] || null
                        logScale: true
                        decimals: 2
                        defaultValue: 1.0
                        formatter: editor.formatOf(param)
                        parser: text => param ? param.parse(text) : null
                        sampleText: "10.00 Hz"
                        tooltip: qsTr("LFO rate")
                    }
                    ParamChoice {
                        objectName: "lfoBeats"
                        visible: editor.on("lfo_sync")
                        anchors.fill: parent
                        param: editor.params["lfo_beats"] || null
                        tooltip: qsTr("LFO rate: a cycle every this long")
                    }
                }
            }

            // The envelope: Classic's ADSR, the others' fades.
            Row {
                visible: editor.mode === 0
                Cell { paramId: "attack" }
                Cell { paramId: "decay" }
                Cell { paramId: "sustain" }
                Cell { paramId: "release" }
            }
            Row {
                visible: editor.mode !== 0
                Cell { paramId: "fade_in" }
                Cell { paramId: "fade_out" }
            }

            Cell {
                paramId: "tune"
                title: qsTr("Transp")
            }
            Cell {
                paramId: "velocity"
            }
            Cell {
                paramId: "volume"
            }
        }
    }

    // --- Controls ------------------------------------------------------------------------

    Row {
        id: controlsPage
        objectName: "controlsPage"
        x: editor.margin
        y: 6
        visible: editor.page === 1
        spacing: 22

        Section {
            title: qsTr("Pitch")
            Grid {
                columns: 3
                rowSpacing: 4
                Cell { paramId: "root"; wide: true }
                Cell { paramId: "tune"; title: qsTr("Transp"); wide: true }
                Cell { paramId: "fine"; wide: true }
                Cell { paramId: "voices"; wide: true }
                Cell { paramId: "glide"; wide: true }
            }
        }
        Section {
            title: qsTr("Sample")
            Row {
                spacing: 4
                Grid {
                    columns: 2
                    rowSpacing: 4
                    Cell { paramId: "start"; wide: true }
                    Cell { paramId: "end"; wide: true }
                    Cell { paramId: "loop_start"; wide: true }
                    Cell { paramId: "loop_fade"; wide: true }
                }
                Column {
                    spacing: 3
                    topPadding: 4
                    ParamButton {
                        objectName: "reverse"
                        width: 54
                        param: editor.params["reverse"] || null
                        text: qsTr("Reverse")
                        tooltip: qsTr("Play the sample backwards")
                    }
                    ParamButton {
                        objectName: "snapControls"
                        width: 54
                        param: editor.params["snap"] || null
                        text: qsTr("Snap")
                        tooltip: qsTr("Start, End, Loop Start and slices at zero crossings")
                    }
                    ParamButton {
                        objectName: "loopControls"
                        width: 54
                        param: editor.params["loop"] || null
                        text: qsTr("Loop")
                        tooltip: qsTr("Classic: loop from Loop Start to End while the note holds")
                    }
                }
            }
        }
        Section {
            title: qsTr("LFO")
            Row {
                spacing: 6
                Column {
                    spacing: 3
                    topPadding: 4
                    Row {
                        spacing: 2
                        ParamButton {
                            objectName: "lfoControls"
                            width: 34
                            param: editor.params["lfo"] || null
                            text: qsTr("On")
                            tooltip: qsTr("The LFO on or off")
                        }
                        ParamButton {
                            objectName: "lfoRetrig"
                            width: 40
                            param: editor.params["lfo_retrig"] || null
                            text: qsTr("Retrig")
                            tooltip: qsTr("Each note starts the LFO from the start of its cycle")
                        }
                    }
                    ParamChoice {
                        objectName: "lfoWaveControls"
                        width: 76
                        param: editor.params["lfo_wave"] || null
                        icons: ["wave_sine", "wave_triangle", "wave_saw_up", "wave_saw_down", "wave_square",
                                "wave_random"]
                    }
                    ParamChoice {
                        objectName: "lfoSyncControls"
                        width: 76
                        param: editor.params["lfo_sync"] || null
                        labels: [qsTr("Hz"), qsTr("Synced")]
                        tooltip: qsTr("Its rate in Hz, or synced to the song")
                    }
                    Item {
                        width: 76
                        height: 16
                        ParamBox {
                            visible: !editor.on("lfo_sync")
                            anchors.fill: parent
                            param: editor.params["lfo_rate"] || null
                            logScale: true
                            decimals: 2
                            defaultValue: 1.0
                            formatter: editor.formatOf(param)
                            parser: text => param ? param.parse(text) : null
                            sampleText: "10.00 Hz"
                            tooltip: qsTr("LFO rate")
                        }
                        ParamChoice {
                            visible: editor.on("lfo_sync")
                            anchors.fill: parent
                            param: editor.params["lfo_beats"] || null
                            tooltip: qsTr("LFO rate: a cycle every this long")
                        }
                    }
                }
                Grid {
                    columns: 2
                    rowSpacing: 4
                    Cell { paramId: "lfo_volume"; title: qsTr("Volume") }
                    Cell { paramId: "lfo_pitch"; title: qsTr("Pitch") }
                    Cell { paramId: "lfo_filter"; title: qsTr("Filter") }
                    Cell { paramId: "lfo_pan"; title: qsTr("Pan") }
                }
            }
        }
        Section {
            title: qsTr("Output")
            Grid {
                columns: 1
                rowSpacing: 4
                Cell { paramId: "pan" }
                Cell { paramId: "gain" }
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Load Sample")
        nameFilters: [qsTr("Audio Files (*.wav *.wave *.flac *.mp3)")]
        onAccepted: view.loadSample(FileUrls.localPath(selectedFile))
    }
}
