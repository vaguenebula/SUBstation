pragma Singleton
import QtQuick

// The built-in devices' own editors: a device of one of these kinds shows its
// editor instead of a knob per parameter (DeviceParamKnob, in pages).
//
//   const url = DeviceEditors.editorFor(device.kind)   // "" for the generic knobs
//   Loader { Component.onCompleted: setSource(url, { trackId: t, deviceId: d }) }
//
// What an editor is, for the device view that hosts it:
// - `required property string trackId` and `deviceId`: the device it edits
//   (on its track, the master or a return; in a rack too).
// - It is the device's body: the frame around it (its border, the title bar
//   with its fold, on/off, name, sidechain, page and save buttons, the menu)
//   is the view's. Its `implicitWidth` is the body's width (the device's less
//   its 1 px border either side); it may change (the EQ's band controls shown
//   or collapsed, in every EQ at once). Give it the body's whole height: it
//   grows its graphs into it; `implicitHeight` is the least it needs.
// - Optional: `pages` (read) and `page` (read/write): its pages of knobs (the
//   Sampler's two); the title bar shows the page arrows while there is more
//   than one. `menuActions` (a list of Action): what the device's right-click
//   menu starts with (the Sampler's Load Sample… and Clear Sample).
//   `sidechainMenuRequested()`: show the device's sidechain menu (the
//   Sidechain's hint over its curve).
// - It reads its displays as the meters update (EngineBridge::metersUpdated)
//   while it is visible, and edits through the editor (undoably). Clicks it
//   doesn't take (on its background) go on to what is under it: the view's
//   frame (selecting the device, its menu).
QtObject {
    // kind -> the editor's file, beside this one.
    readonly property var editors: ({
        "compressor": "CompressorEditor.qml",
        "delay": "DelayEditor.qml",
        "disperser": "DisperserEditor.qml",
        "eq": "EqEditor.qml",
        "sampler": "SamplerEditor.qml",
        "sidechain": "SidechainEditor.qml"
    })

    // The editor's component URL for built-in devices of `kind`, or "" when it has none of its own.
    function editorFor(kind) {
        const file = Object.prototype.hasOwnProperty.call(editors, kind) ? editors[kind] : ""
        return file ? Qt.resolvedUrl(file).toString() : ""
    }
}
