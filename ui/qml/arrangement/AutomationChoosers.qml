import QtQuick
import QtQuick.Controls
import SUBstation

// A strip's automation choosers, as Ableton's, in its header's name column
// (`left`..`right`): for its own lane, from `mainTop`, a device chooser
// ("Mixer" or one of its devices), under it a parameter chooser, and under
// that "+" (another lane below); for each lane below, its two choosers at the
// lane's top and "−" (remove it) beside the parameter's. Automated parameters
// are marked in the menus. Fills its header.
Item {
    id: choosers

    required property TrackHeaderItem header
    required property ArrangementMenu menu
    property real columnLeft
    property real columnRight
    property real mainTop

    readonly property int chooserHeight: 16
    readonly property int pitch: 18
    readonly property int buttonSize: 16

    function laneTop(lane) {
        const lanes = header.lanes
        for (let i = 0; i < lanes.length; ++i) {
            if (lanes[i].index === lane)
                return lanes[i].top + Math.max(2, (lanes[i].height - chooserHeight - pitch) / 2)
        }
        return 0
    }

    Repeater {
        model: choosers.header.choosers

        Item {
            id: controls

            required property var modelData
            readonly property int lane: modelData.lane
            // A lane's parameter chooser makes room for its "−".
            readonly property real paramWidth: lane < 0 ? width : width - choosers.buttonSize - 3

            x: choosers.columnLeft
            y: lane < 0 ? choosers.mainTop : Math.floor(choosers.laneTop(lane))
            width: Math.max(20, choosers.columnRight - choosers.columnLeft)
            height: choosers.pitch + choosers.chooserHeight + (lane < 0 ? choosers.pitch : 0)

            AutomationChooser {
                objectName: "deviceChooser"
                width: controls.width
                text: controls.modelData.device
                tooltip: qsTr("Device (or the mixer) to show automation of")
                onClicked: choosers.menu.show(choosers.header.deviceMenuEntries(controls.lane), choosers.header,
                                              this, 0, height)
            }
            AutomationChooser {
                objectName: "paramChooser"
                y: choosers.pitch
                width: controls.paramWidth
                text: controls.modelData.param
                tooltip: qsTr("Parameter to show automation of")
                onClicked: choosers.menu.show(choosers.header.paramMenuEntries(controls.lane), choosers.header,
                                              this, 0, height)
            }
            RoleButton {
                objectName: controls.lane < 0 ? "addLane" : "removeLane"
                x: controls.width - choosers.buttonSize
                y: controls.lane < 0 ? 2 * choosers.pitch : choosers.pitch
                width: choosers.buttonSize
                height: choosers.buttonSize
                role: "small"
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                text: controls.lane < 0 ? "+" : "−"
                font.pointSize: 9
                font.weight: 700
                tooltip: controls.lane < 0 ? qsTr("Show automation in a new lane") : qsTr("Remove this lane")
                background: Rectangle {
                    radius: controls.lane < 0 ? width / 2 : 2
                    color: parent.hovered ? Theme.surfaceHover : Theme.surface
                    border.color: Theme.border
                }
                onClicked: controls.lane < 0 ? choosers.header.addLane() : choosers.header.removeLane(controls.lane)
            }
        }
    }
}
