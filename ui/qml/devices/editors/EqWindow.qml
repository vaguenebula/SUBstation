import QtQuick
import QtQuick.Controls
import SUBstation

// The EQ, bigger, in a window of its own: the curve over a bar with the
// selected band's controls, the analyzer reading the displays on a timer of its
// own. It stays open as the device view changes, is titled after the device's
// track, and closes when the device goes. EqWindows opens one per device.
Window {
    id: window

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph

    width: 1100
    height: 520
    title: graph.trackName !== "" ? qsTr("EQ — %1").arg(graph.trackName) : qsTr("EQ")
    color: Theme.panel

    Item {
        anchors.fill: parent
        anchors.margins: 10

        EqGraph {
            id: graph
            objectName: "eqGraph"
            session: Session
            trackId: window.trackId
            deviceId: window.deviceId
            displayInterval: 16
            width: parent.width
            height: parent.height - 8 - panel.height
            onAliveChanged: if (!alive) window.close()

            EqCorner {
                graph: graph
                paramId: "scale"
                title: qsTr("Scale")
                atLeft: true
                x: 4
                y: graph.height - height - 4
            }
            EqCorner {
                graph: graph
                paramId: "output"
                title: qsTr("Output")
                atLeft: false
                x: graph.width - width - 4
                y: graph.height - height - 4
            }
        }

        EqBandPanel {
            id: panel
            graph: graph
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: 162
        }

        EqGraphMenus {
            graph: graph
        }
    }
}
