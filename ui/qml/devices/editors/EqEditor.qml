import QtQuick
import QtQuick.Controls
import SUBstation

// The EQ's editor in the device view: the curve (EqGraph) from the title bar to
// the bottom edge, Scale and Output in its bottom corners, the expand button
// (the EQ, bigger, in a window of its own: EqWindows) and the button showing
// the selected band's controls (EqBandPanel) beside the curve. Those start
// collapsed, and showing them shows them in every EQ (EqView.panel): the editor
// is wider then (implicitWidth).
Item {
    id: editor

    required property string trackId
    required property string deviceId
    readonly property alias graph: graph
    readonly property alias panel: panel

    // The device's body: GRAPH_WIDTH, and with the band's controls SPACING + PANEL_WIDTH + PANEL_INSET.
    implicitWidth: EqView.panel ? 580 + 8 + 162 + 6 : 580
    implicitHeight: 60

    EqGraph {
        id: graph
        objectName: "eqGraph"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        width: EqView.panel ? editor.width - 8 - panel.width - 6 : editor.width
        height: editor.height
        rightInset: 2 * (expand.width + 4)

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

        RoleButton {
            id: expand
            objectName: "expand"
            x: graph.width - width - 4
            y: 4
            width: 18
            height: 16
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            iconName: "expand"
            iconSize: 10
            tooltip: qsTr("Open in a window")
            onClicked: EqWindows.open(editor.trackId, editor.deviceId)
        }
        RoleButton {
            id: panelButton
            objectName: "panelButton"
            x: expand.x - width - 4
            y: 4
            width: 18
            height: 16
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            iconName: "sliders"
            iconSize: 10
            checkable: false
            checked: EqView.panel
            tooltip: EqView.panel ? qsTr("Collapse the band's controls") : qsTr("Show the band's controls")
            onClicked: EqView.panel = !EqView.panel
        }
    }

    EqBandPanel {
        id: panel
        graph: graph
        visible: EqView.panel
        x: graph.width + 8
        y: 5
        width: 162
        height: editor.height - 5 - 4
    }

    EqGraphMenus {
        graph: graph
    }
}
