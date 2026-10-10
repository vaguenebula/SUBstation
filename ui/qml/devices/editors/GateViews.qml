pragma Singleton
import QtQuick

// Which Gates show their sidechain section, by device id: view state, not saved
// (as Ableton's unfolded sidechain isn't part of a set's sound), kept here so it
// outlives the frames being made again when the chain changes.
QtObject {
    // device id -> true while its section shows
    property var shown: ({})
    // Bumped on every change, so bindings that ask isShown() read it again.
    property int revision: 0

    function isShown(deviceId) {
        return revision >= 0 && shown[deviceId] === true
    }

    function setShown(deviceId, on) {
        shown[deviceId] = on
        ++revision
    }
}
