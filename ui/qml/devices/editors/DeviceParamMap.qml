import QtQuick
import SUBstation

// A device editor's parameters by id: a DeviceParam for each of `ids`, made once.
//
//   DeviceParamMap { id: p; trackId: editor.trackId; deviceId: editor.deviceId; ids: ["drive", "type"] }
//   EditorKnob { param: p.get("drive") }   // or p.params["drive"]
QtObject {
    id: map

    property string trackId
    property string deviceId
    property var ids: []
    // id -> DeviceParam, made again when the parameters are (bindings on it follow).
    readonly property var params: {
        const all = {}
        for (let i = 0; i < instantiator.count; ++i) {
            const p = instantiator.objectAt(i)
            if (p)
                all[p.paramId] = p
        }
        return all
    }

    // The parameter `id` (null while there is none).
    function get(id) {
        return params[id] || null
    }

    readonly property Instantiator instantiator: Instantiator {
        model: map.ids
        DeviceParam {
            required property string modelData
            session: Session
            trackId: map.trackId
            deviceId: map.deviceId
            paramId: modelData
        }
    }
}
