import QtQuick
import QtQuick.Controls
import SUBstation

// A strip's automation choosers (automation_header.py's AutomationControls),
// as in Ableton: for its own lane a device chooser ("Mixer" or one of its
// devices), a parameter chooser and "+" (another lane below); for each lane
// below its choosers and "−" (remove it). Automated parameters are marked in
// the menus. Fills its header; `mainRect` is where its own lane's go, and the
// lanes' go across `laneLeft`..`laneRight`, centred in each lane.
Item {
    id: choosers

    required property TrackHeaderItem header
    required property ArrangementMenu menu
    property rect mainRect
    property real laneLeft
    property real laneRight

    readonly property int chooserHeight: 18
    readonly property int buttonSize: 18

    function laneTop(lane) {
        const lanes = header.lanes
        for (let i = 0; i < lanes.length; ++i) {
            if (lanes[i].index === lane)
                return lanes[i].top + (lanes[i].height - chooserHeight) / 2
        }
        return 0
    }

    Repeater {
        model: choosers.header.choosers

        Item {
            id: controls

            required property var modelData
            readonly property int lane: modelData.lane
            readonly property real rowLeft: lane < 0 ? choosers.mainRect.x : choosers.laneLeft
            readonly property real rowWidth: lane < 0 ? choosers.mainRect.width : choosers.laneRight - choosers.laneLeft
            readonly property int chooserWidth: Math.floor((rowWidth - choosers.buttonSize - 8) / 2)

            x: rowLeft
            y: lane < 0 ? Math.floor(choosers.mainRect.y + (choosers.mainRect.height - choosers.chooserHeight) / 2)
                        : Math.floor(choosers.laneTop(lane))
            width: rowWidth
            height: choosers.chooserHeight

            AutomationChooser {
                objectName: "deviceChooser"
                width: controls.chooserWidth
                text: controls.modelData.device
                tooltip: qsTr("Device (or the mixer) to show automation of")
                onClicked: choosers.menu.show(choosers.header.deviceMenuEntries(controls.lane), choosers.header,
                                              this, 0, height)
            }
            AutomationChooser {
                objectName: "paramChooser"
                x: controls.chooserWidth + 4
                width: controls.chooserWidth
                text: controls.modelData.param
                tooltip: qsTr("Parameter to show automation of")
                onClicked: choosers.menu.show(choosers.header.paramMenuEntries(controls.lane), choosers.header,
                                              this, 0, height)
            }
            RoleButton {
                objectName: controls.lane < 0 ? "addLane" : "removeLane"
                x: controls.width - choosers.buttonSize
                width: choosers.buttonSize
                height: choosers.chooserHeight
                role: "small"
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                text: controls.lane < 0 ? "+" : "−"
                font.pointSize: 10
                font.weight: 600
                tooltip: controls.lane < 0 ? qsTr("Show automation in a new lane") : qsTr("Remove this lane")
                onClicked: controls.lane < 0 ? choosers.header.addLane() : choosers.header.removeLane(controls.lane)
            }
        }
    }
}
