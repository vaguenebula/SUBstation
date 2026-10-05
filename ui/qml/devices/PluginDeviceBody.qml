import QtQuick
import QtQuick.Controls
import SUBstation

// The body of a plug-in device: its parameters (those a generic editor offers:
// PluginParams), four to a page in a 2×2 grid, the device's title bar paging
// through them (`pages`, `page`); or what it says instead: that it is loading
// (a project just opened), why it didn't load (missing, failed: it keeps its
// place), or that it has no parameters to show (use its own editor). The
// message is cut to four lines (MESSAGE_LINES), so a long one keeps the device
// as tall as the others; its tooltip has it all.
Item {
    id: body

    required property string trackId
    required property string deviceId
    required property DeviceInfo info
    readonly property int perPage: 4
    readonly property int columns: 2
    readonly property int pages: Math.max(1, Math.ceil((params.loaded ? params.count : 0) / perPage))
    property int page: 0
    readonly property alias params: params
    readonly property alias message: message

    implicitWidth: 216 - 2
    implicitHeight: grid.y + grid.implicitHeight + 6

    PluginParams {
        id: params
        session: Session
        trackId: body.trackId
        deviceId: body.deviceId
    }

    Text {
        id: message
        objectName: "pluginMessage"
        x: 8
        y: 6
        width: body.width - 16
        visible: text !== ""
        text: !body.info.loaded ? body.info.pluginMessage
                                : (params.count === 0 ? qsTr("No parameters to show here: use the plug-in's editor.") : "")
        wrapMode: Text.Wrap
        maximumLineCount: 4
        elide: Text.ElideRight
        color: Theme.textDim
        font: Theme.font

        HoverHandler {
            id: messageHover
        }
        ToolTip.visible: messageHover.hovered && message.text !== ""
        ToolTip.text: message.text
        ToolTip.delay: 700
    }

    Grid {
        id: grid
        x: 8
        y: message.visible ? message.y + message.height + 4 : 6
        columns: body.columns
        columnSpacing: 16
        rowSpacing: 6

        Repeater {
            model: params.loaded ? params.indices.slice(body.page * body.perPage, (body.page + 1) * body.perPage) : []
            PluginParamKnob {
                required property int modelData
                objectName: "param_" + modelData
                trackId: body.trackId
                deviceId: body.deviceId
                index: modelData
            }
        }
    }
}
