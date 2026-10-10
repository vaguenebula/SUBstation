import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import SUBstation

// The Gate's editor, laid out as Ableton's Gate: the display (GateGraph: the
// levels scrolling by, the input light and the output darker and outlined over
// it, the threshold and return lines to drag), then Threshold, Return and Floor
// over Attack, Hold and Release, and Flip and Lookahead beside them. The strip
// at the left unfolds the sidechain section, as Ableton's unfolds to the left of
// the device: where the key comes from (the device's sidechain menu), its Gain
// and Dry/Wet, listening to it, and an EQ on it (six types, Freq, Q and Gain,
// and its curve: GateKeyGraph). Every control shows its parameter as it is now
// (its automation's value while that plays), sets it undoably, touches it when
// pressed, and right-click gives its menu. What is set for later (the external
// knobs without a sidechain, the EQ's controls and curve while it is off) is
// dimmed but still settable, as Live's greyed controls; only what the EQ's type
// doesn't use (the shelves' Q, the pass filters' Gain) is disabled.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    // The source button asks the frame for the device's sidechain menu, under it.
    signal sidechainMenuRequested(var from)

    readonly property alias graph: graph
    readonly property alias keyGraph: keyGraph
    readonly property alias lineMenu: lineMenu  // (the threshold's or Return's, from the display's lines)
    // Whether the sidechain section shows: view state (DeviceViews), so a frame made again keeps it.
    readonly property bool sidechainShown: DeviceViews.value(deviceId, "sidechain", false)
    readonly property int foldedWidth: 566
    // The section's knob cells, each as wide as its parameter's widest value needs.
    readonly property int scCellWidth: Math.max(46, Math.ceil(scWidest.advanceWidth))
    readonly property int eqCellWidth: Math.max(52, Math.ceil(eqWidest.advanceWidth))
    readonly property int sectionWidth: 2 * scCellWidth + 6 + 3 * eqCellWidth
    // How far the main panel moves right for the sidechain section: its width, and the divider with
    // the space round it (13 px), in step with the section as it unfolds.
    readonly property real shift: section.width * (sectionWidth + 13) / sectionWidth
    readonly property bool eqOn: p.get("sc_eq") ? p.get("sc_eq").value >= 0.5 : false
    readonly property int eqType: p.get("sc_eq_type") ? p.get("sc_eq_type").index : 5
    readonly property bool listening: p.get("sc_listen") ? p.get("sc_listen").value >= 0.5 : false
    // Whether the key EQ's type uses the gain (the shelves and the bell) and the Q (the bell and the
    // pass filters): the engine's rule (GateDesign.h), as the key graph has it.
    readonly property bool eqUsesGain: keyGraph.usesGain
    readonly property bool eqUsesQ: keyGraph.usesQ

    implicitWidth: foldedWidth + shift
    implicitHeight: 6 + Math.max(knobGrid.implicitHeight, graph.implicitHeight, section.implicitHeight) + 6

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["threshold", "return", "attack", "hold", "release", "floor", "lookahead", "flip", "sc_gain", "sc_mix",
              "sc_listen", "sc_eq", "sc_eq_type", "sc_eq_freq", "sc_eq_q", "sc_eq_gain"]
    }

    TextMetrics {
        id: scWidest
        font: Theme.uiFont(8)  // (EditorReadout's)
        text: "-70.0 dB"
    }
    TextMetrics {
        id: eqWidest
        font: Theme.uiFont(8)
        text: "15.00 kHz"
    }

    // Floor at its bottom is silence (the engine's rule, through the graph).
    function floorText(v) {
        const floor = p.get("floor")
        return graph.floorIsSilent(v) ? "−inf dB" : (floor ? floor.format(v) : "")
    }

    // --- The sidechain section's fold: a strip at the left -------------------------------------

    Rectangle {
        id: fold
        objectName: "sidechainFold"

        // Lit while the key isn't the plain input.
        readonly property color ink: graph.keyed || editor.eqOn || editor.listening ? Theme.accent : Theme.textDim

        x: 8
        y: 6
        width: 14
        height: editor.height - 12
        radius: 3
        color: foldArea.containsMouse ? Theme.surfaceHover : Theme.surface
        Behavior on color {
            ColorAnimation {
                duration: 120
            }
        }

        Icon {
            x: 3
            y: 4
            name: "fold"
            size: 8
            checked: !editor.sidechainShown
            color: fold.ink
        }
        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 4
            rotation: -90
            text: qsTr("Sidechain")
            color: fold.ink
            font: Theme.uiFont(7.5)
            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
        MouseArea {
            id: foldArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: DeviceViews.setValue(editor.deviceId, "sidechain", !editor.sidechainShown)
        }

        ToolTip.visible: foldArea.containsMouse
        ToolTip.delay: 700
        ToolTip.text: qsTr("Sidechain: what opens the gate (another track, its gain and blend),\n"
                           + "an EQ on it, and listening to it")
    }

    // --- The sidechain section -----------------------------------------------------------------

    Item {
        id: section
        objectName: "sidechainSection"
        x: 28
        y: 6
        width: editor.sidechainShown ? editor.sectionWidth : 0
        height: editor.height - 12
        implicitHeight: scKnobs.y + scKnobs.height + 2 + 36  // (the key graph's least)
        clip: true
        visible: width > 0
        Behavior on width {
            NumberAnimation {
                duration: 140
                easing.type: Easing.OutCubic
            }
        }

        // Laid out at its full width: unfolding reveals it rather than squeezing it.
        Item {
            width: editor.sectionWidth
            height: parent.height

            RoleButton {
                id: source
                objectName: "sidechainSource"
                width: 96
                height: 16
                role: "small"
                leftPadding: 4
                rightPadding: 4
                checkable: false
                checked: graph.keyed
                text: graph.sidechainName || qsTr("No Sidechain")
                tooltip: qsTr("Where the key comes from: choose a track to open the gate with\n"
                              + "(the device's sidechain menu)")
                onClicked: editor.sidechainMenuRequested(source)
                // Its name (elided) and an arrow, as a drop-down's.
                contentItem: Item {
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 2
                        anchors.right: sourceArrow.left
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        text: source.text
                        elide: Text.ElideRight
                        font: source.font
                        color: source.look.text
                    }
                    Icon {
                        id: sourceArrow
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        name: "fold"
                        size: 7
                        color: source.look.text
                    }
                }
            }
            ParamButton {
                objectName: "sc_listen"
                x: 102
                width: 20
                param: p.get("sc_listen")
                iconName: "headphones"
                iconSize: 11
                tooltip: qsTr("Listen: hear the key (the sidechain, filtered by the EQ) instead of the output")
            }
            ParamButton {
                objectName: "sc_eq"
                x: 128
                width: 22
                param: p.get("sc_eq")
                text: qsTr("EQ")
                tooltip: qsTr("EQ: open the gate from a band of the key only")
            }
            // The EQ's type, a button each (its shape drawn as the EQ's bands' are), dimmed while the EQ
            // is off.
            Repeater {
                model: [qsTr("Low Shelf"), qsTr("Bell"), qsTr("High Shelf"), qsTr("Low-pass"), qsTr("Band-pass"),
                        qsTr("High-pass")]
                Item {
                    id: typeCell
                    required property int index
                    required property string modelData
                    // The S/C EQ Type's values (Live's order) as the EQ's types.
                    readonly property var kinds: [EqGraph.LowShelf, EqGraph.Bell, EqGraph.HighShelf, EqGraph.HighCut,
                                                  EqGraph.BandPass, EqGraph.LowCut]
                    x: 152 + 17 * index
                    y: 1
                    width: 17
                    height: 15
                    opacity: editor.eqOn ? 1 : 0.55
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 120
                        }
                    }

                    ParamArea {
                        anchors.fill: parent
                        param: p.get("sc_eq_type")
                    }
                    T.AbstractButton {
                        id: typeButton
                        objectName: "eqType" + typeCell.index
                        anchors.fill: parent
                        focusPolicy: Qt.NoFocus
                        hoverEnabled: true
                        checkable: false
                        checked: editor.eqType === typeCell.index
                        onPressed: if (p.get("sc_eq_type")) p.get("sc_eq_type").touch()
                        onClicked: if (p.get("sc_eq_type")) p.get("sc_eq_type").set(typeCell.index)
                        background: EqTypeIcon {
                            kind: typeCell.kinds[typeCell.index]
                            color: Theme.accent
                            checked: typeButton.checked
                            hovered: typeButton.hovered
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 700
                        ToolTip.text: typeCell.modelData
                    }
                }
            }

            Item {
                id: scKnobs
                y: 20
                width: parent.width
                height: scGain.implicitHeight

                // (Without a sidechain they have nothing to act on yet: dimmed, set beforehand.)
                EditorKnob {
                    id: scGain
                    objectName: "sc_gain"
                    width: editor.scCellWidth
                    size: 28
                    param: p.get("sc_gain")
                    title: qsTr("Gain")
                    opacity: graph.keyed ? 1 : 0.55
                    tooltip: qsTr("Sidechain gain: how loud the sidechain is to the gate (never heard)")
                }
                EditorKnob {
                    id: scMix
                    objectName: "sc_mix"
                    x: scGain.width + 2
                    width: editor.scCellWidth
                    size: 28
                    param: p.get("sc_mix")
                    title: qsTr("Dry/Wet")
                    opacity: graph.keyed ? 1 : 0.55
                    tooltip: qsTr("Sidechain Dry/Wet: 100 %: only the sidechain opens the gate;\n"
                                  + "0 %: only the device's own input")
                }
                // Dimmed while the EQ is off, as its type buttons and curve, all still settable; the Q
                // and Gain disabled for a type that doesn't use them.
                EditorKnob {
                    id: eqFreq
                    objectName: "sc_eq_freq"
                    x: scMix.x + scMix.width + 4
                    width: editor.eqCellWidth
                    size: 28
                    param: p.get("sc_eq_freq")
                    title: qsTr("Freq")
                    opacity: editor.eqOn ? 1 : 0.55
                    tooltip: qsTr("Key EQ frequency")
                }
                EditorKnob {
                    id: eqQ
                    objectName: "sc_eq_q"
                    x: eqFreq.x + eqFreq.width
                    width: editor.eqCellWidth
                    size: 28
                    param: p.get("sc_eq_q")
                    title: qsTr("Q")
                    formatter: v => v.toFixed(2)
                    knob.formatter: v => v.toFixed(2)
                    enabled: editor.eqUsesQ
                    opacity: enabled && editor.eqOn ? 1 : 0.55
                    tooltip: qsTr("Key EQ width or resonance (bell, low-, band-, high-pass)")
                }
                EditorKnob {
                    objectName: "sc_eq_gain"
                    x: eqQ.x + eqQ.width
                    width: editor.eqCellWidth
                    size: 28
                    param: p.get("sc_eq_gain")
                    title: qsTr("Gain")
                    knob.bipolar: true
                    enabled: editor.eqUsesGain
                    opacity: enabled && editor.eqOn ? 1 : 0.55
                    tooltip: qsTr("Key EQ gain (shelves and bell)")
                }
            }

            GateKeyGraph {
                id: keyGraph
                objectName: "keyGraph"
                session: Session
                trackId: editor.trackId
                deviceId: editor.deviceId
                y: scKnobs.y + scKnobs.height + 2
                width: parent.width
                height: Math.max(implicitHeight, parent.height - y)

                HoverHandler {
                    id: keyHover
                }
                ToolTip.visible: keyHover.hovered && !keyHover.point.pressedButtons
                ToolTip.delay: 700
                ToolTip.text: qsTr("The EQ on the key: only this band opens the gate.\n"
                                   + "Drag the dot across for the frequency, up and down for the gain (shelves, bell) "
                                   + "or the Q\n(the bell's with Ctrl); the wheel over the dot sets the Q")
            }
        }
    }

    // Between the section and the display.
    EditorDivider {
        x: section.x + section.width + 6
        visible: section.width > 0
        opacity: section.width / editor.sectionWidth
    }

    // --- The main panel (moved right while the section shows) ----------------------------------

    Item {
        id: main
        x: editor.shift
        width: editor.foldedWidth
        height: editor.height

        GateGraph {
            id: graph
            objectName: "gateGraph"
            session: Session
            trackId: editor.trackId
            deviceId: editor.deviceId
            x: 28
            y: 6
            width: 280
            height: Math.max(implicitHeight, editor.height - 12)

            // A line right-clicked: its parameter's menu.
            onParamMenuRequested: id => {
                lineMenu.param = p.get(id)
                lineMenu.show()
            }

            ParamMenu {
                id: lineMenu
            }
            HoverHandler {
                id: graphHover
            }
            ToolTip.visible: graphHover.hovered && !graph.dragging
            ToolTip.delay: 700
            ToolTip.text: qsTr("The last 2.5 s: the input (light) and the output (dark, outlined),\n"
                               + "shaded blue where the gate lets sound through.\n"
                               + "Drag the blue line for the threshold, the orange one for where it closes again "
                               + "(Return);\ndouble-click either to reset it, right-click it for its menu.\n"
                               + "At the right: the input's level and how far the gate turns it down")
        }

        Grid {
            id: knobGrid
            x: 318
            y: Math.max(6, Math.round((editor.height - implicitHeight) / 2))
            columns: 3
            columnSpacing: 4
            rowSpacing: 6

            EditorKnob {
                id: thresholdCell
                objectName: "threshold"
                width: 56
                param: p.get("threshold")
                title: qsTr("Threshold")
                tooltip: qsTr("Threshold: the level at which the gate opens (the blue line)")
            }
            EditorKnob {
                objectName: "return"
                width: 56
                param: p.get("return")
                title: qsTr("Return")
                tooltip: qsTr("Return: how far below the threshold the level must fall before the gate closes "
                              + "again\n(the orange line); more stops chatter")
            }
            EditorKnob {
                objectName: "floor"
                width: 56
                param: p.get("floor")
                title: qsTr("Floor")
                formatter: v => editor.floorText(v)
                knob.formatter: v => editor.floorText(v)
                tooltip: qsTr("Floor: how far a closed gate turns the sound down "
                              + "(−inf: silence; 0 dB: not at all)")
            }
            EditorKnob {
                id: attackCell
                objectName: "attack"
                width: 56
                param: p.get("attack")
                title: qsTr("Attack")
                tooltip: qsTr("Attack: how long the gate takes to open; very short can click, "
                              + "long softens the onset")
            }
            EditorKnob {
                objectName: "hold"
                width: 56
                param: p.get("hold")
                title: qsTr("Hold")
                tooltip: qsTr("Hold: how long the gate stays open after the level falls below the return line")
            }
            EditorKnob {
                objectName: "release"
                width: 56
                param: p.get("release")
                title: qsTr("Release")
                tooltip: qsTr("Release: how long the gate then takes to close")
            }
        }

        // Beside the knobs: Flip level with the first row's, Lookahead with the second's.
        ParamButton {
            objectName: "flip"
            x: 502
            y: knobGrid.y + thresholdCell.y + thresholdCell.knob.y
               + Math.round((thresholdCell.knob.height - height) / 2)
            width: 56
            param: p.get("flip")
            text: qsTr("Flip")
            tooltip: qsTr("Flip: the gate works in reverse: only what is below the threshold passes")
        }
        EditorCaption {
            x: 502
            y: knobGrid.y + attackCell.y
            width: 56
            text: qsTr("Lookahead")
        }
        ParamChoice {
            id: lookahead
            objectName: "lookahead"
            x: 502
            y: knobGrid.y + attackCell.y + attackCell.knob.y + Math.round((attackCell.knob.height - height) / 2)
            // As wide as the knobs' column, or as its longest choice and the arrow need.
            width: Math.max(56, Math.ceil(Math.max(0, ...names.map(name => lookaheadFont.advanceWidth(name))))
                                + button.leftPadding + button.rightPadding)
            param: p.get("lookahead")
            tooltip: qsTr("Lookahead: the gate sees what comes this much ahead, to open before a transient\n"
                          + "(adds as much latency)")

            FontMetrics {
                id: lookaheadFont
                font: lookahead.button.font
            }
        }
    }
}
