pragma Singleton
import QtQuick

// The EQ windows: one per device, made or brought to the front; a window that
// closes is forgotten (and destroyed). (EqWindow.qml is loaded when the first
// one opens: a singleton naming the module's types would depend on the module
// it is part of.)
QtObject {
    id: windows

    // "<track id>/<device id>" -> its EqWindow.
    property var open_: ({})
    property Component component: null

    // The EQ's window (made, or brought to the front).
    function open(trackId, deviceId) {
        const key = trackId + "/" + deviceId
        let window = open_[key]
        if (!window) {
            if (!component)
                component = Qt.createComponent(Qt.resolvedUrl("EqWindow.qml"))
            if (component.status !== Component.Ready) {
                console.warn(component.errorString())
                return null
            }
            window = component.createObject(null, { trackId: trackId, deviceId: deviceId })
            open_[key] = window
            window.closing.connect(() => {
                delete open_[key]
                Qt.callLater(() => window.destroy())
            })
        }
        window.show()
        window.raise()
        window.requestActivate()
        return window
    }

    // The window of a device, if it has one open.
    function windowOf(trackId, deviceId) {
        return open_[trackId + "/" + deviceId] || null
    }
}
