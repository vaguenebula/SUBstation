import QtQuick
import QtQuick.Controls
import SUBstation

// A captioned knob with a value readout, for one of the controller's knobs
// ("transpose", "detune", "gain", "pan"). With several clips it sits on the
// first clip's value and the readout shows the range of values; turning it
// moves every clip by the same amount. When not `active` (Re-Pitch) it is
// greyed out, its tooltip saying why.
Item {
    id: control

    required property ClipViewController controller
    required property string knob
    property bool active: true
    readonly property var spec: controller.knobs[knob]
    readonly property alias knobItem: dial

    // Under the mouse: the readout (its own tooltip while the clips differ),
    // the knob (it shows its value), or the rest (the control's tooltip).
    readonly property point mouse: hover.point.position
    readonly property bool overKnob: mouse.x >= dial.x && mouse.x < dial.x + dial.width && mouse.y >= dial.y
                                     && mouse.y < dial.y + dial.height
    readonly property bool overReadout: mouse.y >= readout.y

    implicitWidth: Math.max(caption.implicitWidth, dial.width, readout.implicitWidth)
    implicitHeight: column.implicitHeight

    HoverHandler {
        id: hover
    }
    ToolTip.visible: hover.hovered && !(control.overKnob && control.active) && ToolTip.text !== ""
    ToolTip.text: control.overReadout && control.spec.readoutTooltip ? control.spec.readoutTooltip
                                                                       : control.spec.tooltip
    ToolTip.delay: 700

    Column {
        id: column
        width: control.width
        spacing: 1

        Label {
            id: caption
            objectName: "caption"
            width: parent.width
            enabled: control.active
            horizontalAlignment: Text.AlignHCenter
            text: control.spec.caption
            color: enabled ? Theme.textDim : Theme.textDisabled
            font: Theme.uiFont(8)
        }

        Knob {
            id: dial
            objectName: "knob"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 38
            height: 38
            enabled: control.active
            from: control.spec.from
            to: control.spec.to
            defaultValue: control.spec.defaultValue
            bipolar: control.spec.bipolar
            value: control.spec.value
            formatter: v => control.controller.format(control.knob, v)
            onMoved: (value, gestureKey) => control.controller.nudge(control.knob, value, gestureKey)
        }

        Label {
            id: readout
            objectName: "readout"
            width: parent.width
            enabled: control.active
            horizontalAlignment: Text.AlignHCenter
            text: control.spec.text
            font: Theme.uiFont(8)
        }
    }
}
