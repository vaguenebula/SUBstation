import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// File › Export Audio…: what to render (the arrangement, from its start to the
// end of its last clip; the loop region, while the loop is on and has a
// length; or the time selection, while there is one), the file type (WAV or
// MP3), and the WAV's bit depth (16-bit, 24-bit, 32-bit float; 24 at first) or
// the MP3's bitrate (320 kbps at first). The file type and bitrate stay as last
// chosen. OK says so with exportChosen(range, bitDepth, fileType, bitrate); the
// window then asks where (Session.exportProblem first) and exports.
Dialog {
    id: dialog

    property var ranges: []
    property int rangeIndex: 0
    property int bitDepthIndex: 0
    property int fileTypeIndex: 0
    property int bitrateIndex: defaultBitrateIndex()
    readonly property string range: ranges.length > rangeIndex ? ranges[rangeIndex].value : "arrangement"
    readonly property int bitDepth: Session.exportBitDepthChoices[bitDepthIndex].value
    readonly property string fileType: Session.exportFileTypeChoices[fileTypeIndex].value
    readonly property int bitrate: Session.exportBitrateChoices[bitrateIndex].value
    readonly property bool mp3: fileType === "mp3"

    signal exportChosen(string range, int bitDepth, string fileType, int bitrate)

    function defaultBitDepthIndex() {
        const choices = Session.exportBitDepthChoices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === Session.defaultExportBitDepth)
                return i
        return 0
    }

    function defaultBitrateIndex() {
        const choices = Session.exportBitrateChoices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === Session.defaultExportBitrate)
                return i
        return 0
    }

    objectName: "exportDialog"
    title: qsTr("Export Audio")
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.Ok | Dialog.Cancel
    width: Math.max(360, form.implicitWidth + leftPadding + rightPadding)

    onAboutToShow: {
        ranges = Session.exportRangeChoices()
        rangeIndex = 0
        bitDepthIndex = defaultBitDepthIndex()
    }
    onAccepted: exportChosen(range, bitDepth, fileType, bitrate)

    contentItem: GridLayout {
        id: form
        columns: 2
        columnSpacing: 10
        rowSpacing: 6

        Label { text: qsTr("Rendered Range") }
        ChoiceBox {
            objectName: "exportRange"
            Layout.fillWidth: true
            choices: dialog.ranges
            chosenIndex: dialog.rangeIndex
            onChosen: index => dialog.rangeIndex = index
        }
        Label { text: qsTr("File Type") }
        ChoiceBox {
            objectName: "exportFileType"
            Layout.fillWidth: true
            choices: Session.exportFileTypeChoices
            chosenIndex: dialog.fileTypeIndex
            onChosen: index => dialog.fileTypeIndex = index
        }
        Label {
            text: qsTr("Bit Depth")
            visible: !dialog.mp3
        }
        ChoiceBox {
            objectName: "exportBitDepth"
            Layout.fillWidth: true
            visible: !dialog.mp3
            choices: Session.exportBitDepthChoices
            chosenIndex: dialog.bitDepthIndex
            onChosen: index => dialog.bitDepthIndex = index
        }
        Label {
            text: qsTr("Bitrate")
            visible: dialog.mp3
        }
        ChoiceBox {
            objectName: "exportBitrate"
            Layout.fillWidth: true
            visible: dialog.mp3
            choices: Session.exportBitrateChoices
            chosenIndex: dialog.bitrateIndex
            onChosen: index => dialog.bitrateIndex = index
        }
    }
}
