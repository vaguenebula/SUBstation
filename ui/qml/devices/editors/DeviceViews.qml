pragma Singleton
import QtQuick

// The device editors' view state, by device id and name: a section unfolded, a page of fields
// shown. Not saved (it is how a device is looked at, not its sound), and kept here, not in the
// editor, so it outlives the frames being made again when the chain changes.
//
//   readonly property bool open: DeviceViews.value(deviceId, "sidechain", false)
//   onClicked: DeviceViews.setValue(deviceId, "sidechain", !open)
QtObject {
    // "<device id>/<name>" -> value
    property var values: ({})
    // Bumped on every change, so bindings that call value() read it again.
    property int revision: 0

    function value(deviceId, name, fallback) {
        const v = revision >= 0 ? values[deviceId + "/" + name] : undefined
        return v === undefined ? fallback : v
    }

    function setValue(deviceId, name, v) {
        values[deviceId + "/" + name] = v
        ++revision
    }
}
