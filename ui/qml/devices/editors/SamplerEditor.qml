import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import SUBstation

// The Sampler's editor: its knobs in pages of six, three to a row (the sample's
// six, then the amplitude's), and beside them its sample's waveform
// (SampleView): what plays of it, the loop, and where the newest note is. Drop
// an audio file on the waveform, or double-click it, to load one; drag its
// Start and End markers. The device's menu starts with Load Sample… and Clear
// Sample (menuActions).
Item {
    id: editor

    required property string trackId
    required property string deviceId
    // The pages of knobs (the device's title bar shows the arrows while there is more than one).
    readonly property int pages: Math.max(1, Math.ceil(params.count / perPage))
    property int page: 0
    // What the device's right-click menu starts with.
    readonly property list<Action> menuActions: [
        Action {
            objectName: "loadSample"
            text: qsTr("Load Sample…")
            onTriggered: editor.browse()
        },
        Action {
            objectName: "clearSample"
            text: qsTr("Clear Sample")
            enabled: view.samplePath !== ""
            onTriggered: view.clearSample()
        }
    ]
    readonly property alias view: view
    readonly property int perPage: 6

    function browse() {
        if (view.sampleFolder.toString() !== "")
            fileDialog.currentFolder = view.sampleFolder
        fileDialog.open()
    }

    // The device's body: DEVICE_WIDTH + PARAM_WIDTH + 16 + 12 + VIEW_WIDTH (a third column, and the waveform),
    // less the frame's border.
    implicitWidth: 216 + 84 + 16 + 12 + 240 - 2
    implicitHeight: 6 + Math.max(knobs.implicitHeight, view.implicitHeight) + 6

    DeviceParams {
        id: params
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
    }

    Grid {
        id: knobs
        x: 8
        y: 6
        columns: 3
        columnSpacing: 16
        rowSpacing: 6

        Repeater {
            model: params.ids.slice(editor.page * editor.perPage, (editor.page + 1) * editor.perPage)
            DeviceParamKnob {
                required property string modelData
                objectName: "param_" + modelData
                trackId: editor.trackId
                deviceId: editor.deviceId
                paramId: modelData
            }
        }
    }

    SampleView {
        id: view
        objectName: "sampleView"
        session: Session
        trackId: editor.trackId
        deviceId: editor.deviceId
        x: editor.width - 8 - width
        y: 6
        width: 240
        height: Math.max(implicitHeight, editor.height - 12)
        onBrowseRequested: editor.browse()
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Load Sample")
        nameFilters: [qsTr("Audio Files (*.wav *.wave *.flac *.mp3)")]
        onAccepted: view.loadSampleUrl(selectedFile)
    }
}
