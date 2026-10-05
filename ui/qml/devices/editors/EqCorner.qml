import QtQuick
import SUBstation

// A knob in a corner of the EQ's curve (eq.py's EqEditor._corner): Scale at
// the bottom left, Output at the bottom right, its name and value beside it on
// a dark rounded plate.
Rectangle {
    id: corner

    required property EqGraph graph
    required property string paramId
    required property string title
    property bool atLeft: true
    readonly property alias knob: knob

    implicitWidth: row.implicitWidth + 4 + (atLeft ? 6 : 4)
    implicitHeight: row.implicitHeight + 4
    radius: 6
    color: Qt.rgba(10 / 255, 11 / 255, 14 / 255, 185 / 255)

    DeviceParam {
        id: parameter
        session: Session
        trackId: corner.graph.trackId
        deviceId: corner.graph.deviceId
        paramId: corner.paramId
    }

    TextMetrics {
        id: widest
        font: Theme.uiFont(8)
        text: "-36.0 dB"
    }

    Row {
        id: row
        x: 4
        y: 2
        spacing: 4
        layoutDirection: corner.atLeft ? Qt.LeftToRight : Qt.RightToLeft

        ParamKnob {
            id: knob
            objectName: "corner_" + corner.paramId
            anchors.verticalCenter: parent.verticalCenter
            size: 24
            param: parameter
            bipolar: corner.paramId === "output"
            setter: (v, key) => corner.graph.setParamValue(corner.paramId, v, key)
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            Text {
                width: widest.advanceWidth
                horizontalAlignment: corner.atLeft ? Text.AlignLeft : Text.AlignRight
                text: corner.title
                color: Theme.textDim
                font: Theme.uiFont(7)
            }
            Text {
                width: widest.advanceWidth
                horizontalAlignment: corner.atLeft ? Text.AlignLeft : Text.AlignRight
                text: parameter.text
                color: Theme.text
                font: Theme.uiFont(8)
            }
        }
    }
}
