import QtQuick
import SUBstation

// The grip between two devices in the device view (as Ableton's): two short
// dark lines, upright in the middle of the gap's height. Put it in the gap.
Item {
    implicitWidth: 5

    Repeater {
        model: 2
        Rectangle {
            required property int index
            x: index * 3
            y: parent.height * 0.21
            width: 2
            height: parent.height * 0.58
            color: Theme.border
        }
    }
}
