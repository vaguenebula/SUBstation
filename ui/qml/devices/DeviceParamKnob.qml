import QtQuick
import SUBstation

// A built-in device's parameter, as a cell of the device view shows it: its
// name on top (elided, the whole name in the tooltip), then a knob with its
// value under it, or, for a parameter that chooses between named values, a
// list. The knob turns logarithmically where the engine says so, is drawn from
// the middle for a range across 0 without a unit (or in semitones or cents),
// moves in whole steps for a stepped parameter, shows the automation dot (red
// while automation plays, grey while overridden) and follows the automation as
// it plays. Pressing it (or the cell) shows its automation in the arrangement;
// right-click for its menu (ParamMenu). Edits go through the editor, one undo
// step per drag.
//
//   DeviceParamKnob { trackId: track; deviceId: device; paramId: "threshold" }
ParamCell {
    id: cell

    property string trackId
    property string deviceId
    property string paramId
    readonly property DeviceParam param: parameter

    menuParam: parameter
    name: parameter.name
    minimum: parameter.minimum
    maximum: parameter.maximum
    defaultValue: parameter.defaultValue
    bipolar: parameter.bipolar
    logScale: parameter.logScale
    stepped: parameter.steps > 0
    value: parameter.value
    text: parameter.text
    automation: parameter.automation
    isList: parameter.isList
    labels: parameter.labels
    listIndex: parameter.index
    formatter: v => parameter.format(v)
    onMoved: (v, key) => parameter.set(v, key)
    onChosen: index => parameter.set(index)
    onTouched: parameter.touch()

    DeviceParam {
        id: parameter
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        paramId: cell.paramId
    }
}
