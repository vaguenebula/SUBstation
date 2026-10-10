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
// pressed, and right-click gives its menu.
Item {
    id: editor

    required property string trackId
    required property string deviceId
    // The source button asks the frame for the device's sidechain menu.
    signal sidechainMenuRequested()

    readonly property alias graph: graph
    readonly property alias keyGraph: keyGraph
    readonly property bool sidechainShown: GateViews.isShown(deviceId)
    readonly property int foldedWidth: 566
    readonly property int sectionWidth: 254
    // How far the main panel moves right for the sidechain section: its width, and the divider with
    // the space round it (13 px), in step with the section as it unfolds.
    readonly property real shift: section.width * (sectionWidth + 13) / sectionWidth
    readonly property bool eqOn: p.get("sc_eq") ? p.get("sc_eq").value >= 0.5 : false
    readonly property int eqType: p.get("sc_eq_type") ? p.get("sc_eq_type").index : 5
    readonly property bool listening: p.get("sc_listen") ? p.get("sc_listen").value >= 0.5 : false
    // The key EQ's types that use the gain (the shelves and the bell) and the Q (the bell and the pass
    // filters), as the engine's (GateDesign.h).
    readonly property bool eqUsesGain: eqType <= 2
    readonly property bool eqUsesQ: eqType !== 0 && eqType !== 2

    implicitWidth: foldedWidth + shift
    implicitHeight: 6 + Math.max(knobGrid.implicitHeight, graph.implicitHeight, section.implicitHeight) + 6

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["threshold", "return", "attack", "hold", "release", "floor", "lookahead", "flip", "sc_gain", "sc_mix",
              "sc_listen", "sc_eq", "sc_eq_type", "sc_eq_freq", "sc_eq_q", "sc_eq_gain"]
    }

    // Floor at its bottom is silence.
    function floorText(v) {
        const floor = p.get("floor")
        return v <= -74.95 ? "−inf dB" : (floor ? floor.format(v) : "")
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
            onClicked: GateViews.setShown(editor.deviceId, !editor.sidechainShown)
        }

        ToolTip.visible: foldArea.containsMouse
        ToolTip.delay: 700
        ToolTip.text: qsTr("Sidechain: what opens the gate (another track, its gain and blend),\nan EQ on it, and listening to it")
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
                tooltip: qsTr("Where the key comes from: choose a track to open the gate with\n(the device's sidechain menu)")
                onClicked: editor.sidechainMenuRequested()
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
            // The EQ's type, a button each (its shape drawn), dimmed while the EQ is off.
            Repeater {
                model: [qsTr("Low Shelf"), qsTr("Bell"), qsTr("High Shelf"), qsTr("Low-pass"), qsTr("Band-pass"),
                        qsTr("High-pass")]
                Item {
                    id: typeCell
                    required property int index
                    required property string modelData
                    x: 152 + 17 * index
                    y: 1
                    width: 17
                    height: 15
                    opacity: editor.eqOn ? 1 : 0.45
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
                        background: GateFilterIcon {
                            type: typeCell.index
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

                EditorKnob {
                    id: scGain
                    objectName: "sc_gain"
                    width: 46
                    size: 28
                    param: p.get("sc_gain")
                    title: qsTr("Gain")
                    enabled: graph.keyed
                    tooltip: qsTr("Sidechain gain: how loud the sidechain is to the gate (never heard)")
                }
                EditorKnob {
                    objectName: "sc_mix"
                    x: 48
                    width: 46
                    size: 28
                    param: p.get("sc_mix")
                    title: qsTr("Dry/Wet")
                    enabled: graph.keyed
                    tooltip: qsTr("Sidechain Dry/Wet: 100 %: only the sidechain opens the gate;\n0 %: only the device's own input")
                }
                EditorKnob {
                    objectName: "sc_eq_freq"
                    x: 104
                    width: 50
                    size: 28
                    param: p.get("sc_eq_freq")
                    title: qsTr("Freq")
                    enabled: editor.eqOn
                    tooltip: qsTr("Key EQ frequency")
                }
                EditorKnob {
                    objectName: "sc_eq_q"
                    x: 154
                    width: 50
                    size: 28
                    param: p.get("sc_eq_q")
                    title: qsTr("Q")
                    formatter: v => v.toFixed(2)
                    knob.formatter: v => v.toFixed(2)
                    enabled: editor.eqOn && editor.eqUsesQ
                    tooltip: qsTr("Key EQ width or resonance (bell, low-, band-, high-pass)")
                }
                EditorKnob {
                    objectName: "sc_eq_gain"
                    x: 204
                    width: 50
                    size: 28
                    param: p.get("sc_eq_gain")
                    title: qsTr("Gain")
                    enabled: editor.eqOn && editor.eqUsesGain
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
                ToolTip.text: qsTr("The EQ on the key: only this band opens the gate.\nDrag the dot across for the frequency, up and down for the gain (shelves, bell) or the Q")
            }
        }
    }

    // Between the section and the display.
    Rectangle {
        x: section.x + section.width + 6
        y: 6
        width: 1
        height: editor.height - 12
        visible: section.width > 0
        opacity: section.width / editor.sectionWidth
        color: Theme.border
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

            HoverHandler {
                id: graphHover
            }
            ToolTip.visible: graphHover.hovered && !graph.dragging
            ToolTip.delay: 700
            ToolTip.text: qsTr("The last 2.5 s: the input (light) and the output (dark, outlined),\nshaded blue where the gate lets sound through.\nDrag the blue line for the threshold, the orange one for where it closes again (Return);\ndouble-click either to reset it. At the right: the input's level and how far the gate turns it down")
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
                tooltip: qsTr("Return: how far below the threshold the level must fall before the gate closes again\n(the orange line); more stops chatter")
            }
            EditorKnob {
                objectName: "floor"
                width: 56
                param: p.get("floor")
                title: qsTr("Floor")
                formatter: v => editor.floorText(v)
                knob.formatter: v => editor.floorText(v)
                tooltip: qsTr("Floor: how far a closed gate turns the sound down (−inf: silence; 0 dB: not at all)")
            }
            EditorKnob {
                id: attackCell
                objectName: "attack"
                width: 56
                param: p.get("attack")
                title: qsTr("Attack")
                tooltip: qsTr("Attack: how long the gate takes to open; very short can click, long softens the onset")
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
            objectName: "lookahead"
            x: 502
            y: knobGrid.y + attackCell.y + attackCell.knob.y + Math.round((attackCell.knob.height - height) / 2)
            width: 56
            param: p.get("lookahead")
            tooltip: qsTr("Lookahead: the gate sees what comes this much ahead, to open before a transient\n(adds as much latency)")
        }
    }
}
