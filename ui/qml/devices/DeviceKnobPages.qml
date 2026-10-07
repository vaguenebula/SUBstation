import QtQuick
import SUBstation

// The body of a built-in device without an editor of its own: a knob per
// parameter (or a list), four to a page in a 2×2 grid (PARAMS_PER_PAGE,
// PARAM_COLUMNS) with rowSpacing between its rows (DevicePanel's deviceHeight
// has room for it), the device's title bar paging through them (`pages`,
// `page`, as an editor's).
Item {
    id: body

    required property string trackId
    required property string deviceId
    readonly property int perPage: 4
    readonly property int columns: 2
    readonly property int pages: Math.max(1, Math.ceil(params.count / perPage))
    property int page: 0
    readonly property alias params: params
    readonly property int rowSpacing: 14

    implicitWidth: 216 - 2
    implicitHeight: 6 + grid.implicitHeight + 6

    DeviceParams {
        id: params
        session: Session
        trackId: body.trackId
        deviceId: body.deviceId
    }

    Grid {
        id: grid
        x: 8
        y: 6
        columns: body.columns
        columnSpacing: 16
        rowSpacing: body.rowSpacing

        Repeater {
            model: params.ids.slice(body.page * body.perPage, (body.page + 1) * body.perPage)
            DeviceParamKnob {
                required property string modelData
                objectName: "param_" + modelData
                trackId: body.trackId
                deviceId: body.deviceId
                paramId: modelData
            }
        }
    }
}
