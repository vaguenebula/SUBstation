import QtQuick
import SUBstation

// A knob bound to a built-in device's parameter (a DeviceParam), as the
// device editors bind theirs: it shows the value as it is now and its
// automation dot, sets it through the editor (one undo step per drag), touches
// it when pressed, and right-click gives its menu. `size` is the knob's; the
// range, scale and default are the parameter's unless set.
Item {
    id: control

    property DeviceParam param: null
    property real size: 34
    property alias from: knob.from
    property alias to: knob.to
    property alias bipolar: knob.bipolar
    property alias logScale: knob.logScale
    property alias step: knob.step
    property alias color: knob.color
    // Value -> text (the parameter's own units by default).
    property var formatter: v => param ? param.format(v) : ""
    readonly property alias knob: knob
    // Sets a user's change instead of the parameter's set() (an editor's own undo text): (value, key) => {}.
    property var setter: null

    implicitWidth: size
    implicitHeight: size

    ParamArea {
        anchors.fill: parent
        param: control.param
    }

    Knob {
        id: knob
        anchors.fill: parent
        from: control.param ? control.param.minimum : 0
        to: control.param ? control.param.maximum : 1
        defaultValue: control.param ? control.param.defaultValue : 0
        logScale: control.param ? control.param.logScale : false
        value: control.param ? control.param.value : 0
        automation: control.param ? control.param.automation : ""
        formatter: v => control.formatter(v)
        onMoved: (v, key) => {
            if (control.setter)
                control.setter(v, key)
            else if (control.param)
                control.param.set(v, key)
        }
        onTouched: if (control.param) control.param.touch()
    }
}
