import QtQuick
import QtQuick.Controls
import SUBstation

// The Compressor's editor: every knob on one page, four to a row, and beside
// them its graph (ReductionGraph): the gain reduction over the last second, the
// In meter (the threshold marked) and the Out meter, reading the device's
// displays as the meters update.
Item {
    id: editor

    required property string trackId
    required property string deviceId

    // The device's body: DEVICE_WIDTH + 2 * (PARAM_WIDTH + 16) + 12 + GRAPH_WIDTH, less the frame's border.
    implicitWidth: 216 + 2 * (84 + 16) + 12 + 232 - 2
    implicitHeight: 6 + Math.max(knobs.implicitHeight, graph.implicitHeight) + 4

    DeviceParams {
        id: params
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
    }

    Grid {
        id: knobs
        x: 8
        y: 6
        columns: 4
        columnSpacing: 16
        rowSpacing: 6

        Repeater {
            model: params.ids
            DeviceParamKnob {
                required property string modelData
                objectName: "param_" + modelData
                trackId: editor.trackId
                deviceId: editor.deviceId
                paramId: modelData
            }
        }
    }

    ReductionGraph {
        id: graph
        objectName: "reductionGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: editor.width - 8 - width
        y: 6
        width: 232
        height: Math.max(implicitHeight, editor.height - 10)

        HoverHandler {
            id: hover
        }
        ToolTip.visible: hover.hovered
        ToolTip.delay: 700
        ToolTip.text: qsTr("Gain reduction grows downward (0–24 dB).\nIn: detector level; the accent notch marks the threshold.\nOut: output level. Meters span −60 to 0 dBFS.")
    }
}
