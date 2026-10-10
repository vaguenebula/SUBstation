import QtQuick
import SUBstation

// A plug-in's parameter in its device's generic editor: its name on top, then a
// knob with the plug-in's own text for its value under it, or a list for one
// that chooses between named values. The knob moves in whole steps for a
// stepped parameter, and is drawn from the middle when its default is the
// middle of its range; it shows the automation dot (the plug-in reports its
// values as automation plays). Pressing it shows its automation in the
// arrangement; right-click for its menu (ParamMenu: automation, and in a rack
// the macros). Edits go through the editor with the value before, one undo step
// per drag.
ParamCell {
    id: cell

    property string trackId
    property string deviceId
    property int index: -1
    readonly property PluginParam param: parameter

    menuParam: target
    name: parameter.name
    minimum: parameter.minimum
    maximum: parameter.maximum
    defaultValue: parameter.defaultValue
    bipolar: parameter.bipolar
    stepped: parameter.steps > 0
    value: parameter.value
    text: parameter.text
    automation: parameter.automation
    isList: parameter.isList
    labels: parameter.labels
    listIndex: parameter.listIndex
    formatter: v => parameter.format(v)
    onMoved: (v, key) => parameter.set(v, key)
    onChosen: index => parameter.set(index)
    onTouched: parameter.touch()

    PluginParam {
        id: parameter
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        index: cell.index
    }
    // The same parameter as the menu sees it (its automation, its macro).
    DeviceParam {
        id: target
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        paramId: parameter.paramId
    }
}
