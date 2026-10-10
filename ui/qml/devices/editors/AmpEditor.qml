import QtQuick
import QtQuick.Controls
import SUBstation

// The Amp's editor, laid out as Ableton's Amp: the seven models as a row of
// buttons (an underline in the model's colour slides to the one chosen) over
// Gain, Bass, Middle, Treble, Presence and Volume, with the Output switch (Mono,
// Dual) and Dry/Wet set apart at the right. Around them, in dark wells, what the
// amp is doing: the tubes glowing as hard as each stage is driven and the pilot
// lamp with the model's name (AmpPanel), the transfer curve with the input's
// peaks riding on it (AmpDriveGraph), the tone stack's curve with a handle per
// tone control to drag (AmpToneGraph), and the output meter. Every control shows
// its parameter as it is now (its automation's value while that plays), sets it
// undoably, touches it when pressed, and right-click gives its menu (the tone
// handles' too: handleMenu).
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias panel: panel
    readonly property alias driveGraph: driveGraph
    readonly property alias toneGraph: toneGraph
    readonly property alias underline: underline
    readonly property alias handleMenu: handleMenu

    readonly property int leftWidth: 140   // the tubes over the drive graph
    readonly property int plateWidth: 368  // the model buttons and the knobs over the tone graph
    readonly property int rightWidth: 80   // Output and Dry/Wet over the lamp ("Mono" 3 px clear of its border)
    readonly property int meterWidth: 10
    readonly property int gap: 10
    readonly property int modelWidth: 50  // a model button's (7 x 50 + 6 x 3 = plateWidth)
    readonly property int modelSpacing: 3
    readonly property int knobWidth: 58   // a knob's cell (6 x 58 + 5 x 4 = plateWidth)
    readonly property int plateX: 8 + leftWidth + gap
    readonly property int rightX: plateX + plateWidth + gap
    readonly property int bottomY: knobRow.y + knobRow.height + 4  // the graphs' row

    readonly property var modelTips: [
        qsTr("Clean: the Brilliant channel of a classic 1960s British amp: bright and chiming, clean until pushed"),
        qsTr("Boost: the same amp's Tremolo channel, with more gain: edgy rhythm and riffs"),
        qsTr("Blues: a bright 1970s guitar amp for country, rock and blues; high Volume adds distortion"),
        qsTr("Rock: a classic 1960s 45-watt rock amp: crunchy, full mids"),
        qsTr("Lead: the Modern channel of a high-gain amp: tight, saturated leads"),
        qsTr("Heavy: the same amp's Vintage channel: looser and dirtier; high Volume adds distortion"),
        qsTr("Bass: a rare 1970s PA: a powerful low end and a fuzzy edge; high Volume adds distortion")
    ]

    // The device's body: 8 + the three columns 10 apart + 8 + the meter + 8.
    implicitWidth: rightX + rightWidth + 8 + meterWidth + 8
    implicitHeight: bottomY + 40 + 6  // the graphs at least 40 px tall

    DeviceParamMap {
        id: p
        trackId: editor.trackId
        deviceId: editor.deviceId
        ids: ["type", "gain", "bass", "middle", "treble", "presence", "volume", "dual", "mix"]
    }

    // The face's wells behind everything: the tubes, the lamp and the meter.
    AmpPanel {
        id: panel
        objectName: "ampPanel"
        anchors.fill: parent
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        tubeRect: Qt.rect(8, 6, editor.leftWidth, editor.bottomY - 10)
        jewelRect: Qt.rect(editor.rightX, editor.bottomY, editor.rightWidth, editor.height - editor.bottomY - 6)
        meterRect: Qt.rect(editor.rightX + editor.rightWidth + 8, 6, editor.meterWidth, editor.height - 12)
    }
    Item {  // the tube window's tooltip
        x: 8
        y: 6
        width: editor.leftWidth
        height: editor.bottomY - 10
        HoverHandler {
            id: tubeHover
        }
        ToolTip.visible: tubeHover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("The three preamp stages and the power tube glow as hard as each is driven; "
                           + "the power tube turns blue as the supply sags")
    }
    Item {  // the lamp's
        objectName: "lampTip"
        x: panel.jewelRect.x
        y: panel.jewelRect.y
        width: panel.jewelRect.width
        height: panel.jewelRect.height
        HoverHandler {
            id: lampHover
        }
        ToolTip.visible: lampHover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("The pilot lamp brightens with the output and dims as the supply sags")
    }
    Item {  // the meter's, its whole height
        objectName: "meterTip"
        x: panel.meterRect.x
        y: panel.meterRect.y
        width: panel.meterRect.width
        height: panel.meterRect.height
        HoverHandler {
            id: meterHover
        }
        ToolTip.visible: meterHover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("The output level, after Dry/Wet")
    }

    // The models, Ableton's row of buttons.
    Row {
        id: modelRow
        x: editor.plateX
        y: 6
        spacing: editor.modelSpacing

        Repeater {
            model: p.get("type") ? p.get("type").labels : []
            ParamButton {
                required property string modelData
                required property int index
                objectName: "type_" + modelData.toLowerCase()
                width: editor.modelWidth
                height: 18
                role: "monitor"
                param: p.get("type")
                choice: index
                text: modelData
                tooltip: editor.modelTips[index] || ""
            }
        }
    }
    // Under the chosen model, in its colour: it slides to a new one as its colour turns.
    Rectangle {
        id: underline
        objectName: "typeUnderline"
        x: modelRow.x + (p.get("type") ? p.get("type").index : 0) * (editor.modelWidth + editor.modelSpacing)
        y: modelRow.y + 19
        width: editor.modelWidth
        height: 2
        radius: 1
        color: panel.modelColor
        Behavior on x {
            NumberAnimation {
                duration: 160
                easing.type: Easing.OutCubic
            }
        }
    }

    // Output: one amp for both channels, or one each.
    Row {
        id: outputRow
        x: editor.rightX
        y: 6
        spacing: 4

        ParamButton {
            objectName: "dual_mono"
            width: (editor.rightWidth - 4) / 2
            height: 18
            role: "monitor"
            param: p.get("dual")
            choice: 0
            text: qsTr("Mono")
            tooltip: qsTr("Output Mono: both channels summed through one amp (half the CPU)")
        }
        ParamButton {
            objectName: "dual_dual"
            width: (editor.rightWidth - 4) / 2
            height: 18
            role: "monitor"
            param: p.get("dual")
            choice: 1
            text: qsTr("Dual")
            tooltip: qsTr("Output Dual: each channel through an amp of its own: stereo, twice the CPU")
        }
    }

    Row {
        id: knobRow
        x: editor.plateX
        y: 28
        spacing: 4

        EditorKnob {
            objectName: "gain"
            width: editor.knobWidth
            param: p.get("gain")
            title: qsTr("Gain")
            tooltip: qsTr("Gain: the level into the preamp: the main control of how much the amp distorts")
        }
        EditorKnob {
            objectName: "bass"
            width: editor.knobWidth
            param: p.get("bass")
            title: qsTr("Bass")
            tooltip: qsTr("Bass: the tone stack's lows. The tone controls interact, as on a real amp, "
                          + "and more of them can mean more distortion")
        }
        EditorKnob {
            objectName: "middle"
            width: editor.knobWidth
            param: p.get("middle")
            title: qsTr("Middle")
            tooltip: qsTr("Middle: the tone stack's mids. The tone controls interact, as on a real amp, "
                          + "and more of them can mean more distortion")
        }
        EditorKnob {
            objectName: "treble"
            width: editor.knobWidth
            param: p.get("treble")
            title: qsTr("Treble")
            tooltip: qsTr("Treble: the tone stack's highs. The tone controls interact, as on a real amp, "
                          + "and more of them can mean more distortion")
        }
        EditorKnob {
            objectName: "presence"
            width: editor.knobWidth
            param: p.get("presence")
            title: qsTr("Presence")
            tooltip: qsTr("Presence: the power amp's mid/high edge and crispness")
        }
        EditorKnob {
            objectName: "volume"
            width: editor.knobWidth
            param: p.get("volume")
            title: qsTr("Volume")
            tooltip: qsTr("Volume: the power amp's level. On Blues, Heavy and Bass, turned up, it distorts too")
        }
    }
    EditorKnob {
        objectName: "mix"
        x: editor.rightX
        y: knobRow.y
        width: editor.rightWidth
        param: p.get("mix")
        title: qsTr("Dry/Wet")
        tooltip: qsTr("Dry/Wet: the amp's sound blended with the input")
    }

    AmpDriveGraph {
        id: driveGraph
        objectName: "ampDriveGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: 8
        y: editor.bottomY
        width: editor.leftWidth
        height: Math.max(implicitHeight, editor.height - y - 6)

        HoverHandler {
            id: driveHover
        }
        ToolTip.visible: driveHover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("What comes out for each input level at these settings; the dots ride the input's peaks")
    }
    AmpToneGraph {
        id: toneGraph
        objectName: "ampToneGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: editor.plateX
        y: editor.bottomY
        width: editor.plateWidth
        height: Math.max(implicitHeight, editor.height - y - 6)

        HoverHandler {
            id: toneHover
        }
        ToolTip.visible: toneHover.hovered && !toneHover.point.pressedButtons && toneGraph.dragging < 0
        ToolTip.delay: 700
        ToolTip.text: qsTr("The tone stack and Presence as they sound. Drag B, M, T or P up and down, "
                           + "or turn the wheel over one, to set it; double-click one to put it back to 5; "
                           + "right-click one for its menu")

        onHandleMenuRequested: id => {
            handleMenu.param = p.get(id)
            handleMenu.show()
        }
    }
    // A tone handle's parameter's menu, as its knob's.
    ParamMenu {
        id: handleMenu
    }
}
