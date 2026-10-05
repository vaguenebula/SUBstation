import QtQuick
import QtQuick.Controls
import SUBstation

// A value box bound to a built-in device's parameter (delay.py's _box): 18 px
// high in the 8 pt font; it shows the value as it is now and its automation
// dot, sets it through the editor (one undo step per drag), touches it when
// pressed, and right-click gives its menu. `logScale` drags it evenly in
// log(value) (delay.py's LogValueBox, for frequencies).
Item {
    id: control

    property DeviceParam param: null
    property alias from: box.from
    property alias to: box.to
    property alias step: box.step
    property alias decimals: box.decimals
    property alias defaultValue: box.defaultValue
    property alias logScale: box.logScale
    property alias sampleText: box.sampleText
    property alias formatter: box.formatter
    property alias parser: box.parser
    property string tooltip: ""
    readonly property alias box: box

    implicitWidth: box.implicitWidth
    implicitHeight: 18

    ParamArea {
        anchors.fill: parent
        param: control.param
    }

    ValueBox {
        id: box
        anchors.fill: parent
        font: Theme.uiFont(8)
        from: control.param ? control.param.minimum : 0
        to: control.param ? control.param.maximum : 1
        value: control.param ? control.param.value : 0
        automation: control.param ? control.param.automation : ""
        onMoved: (v, key) => {
            if (control.param)
                control.param.set(v, key)
        }
        onTouched: if (control.param) control.param.touch()

        HoverHandler {
            id: hover
        }
        ToolTip.visible: hover.hovered && !box.dragging && control.tooltip !== ""
        ToolTip.text: control.tooltip
        ToolTip.delay: 700
    }
}
