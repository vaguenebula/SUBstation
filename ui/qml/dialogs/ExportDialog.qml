import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// File › Export Audio…: what to render (the arrangement, from its start to the
// end of its last clip; or the loop region, while the loop is on and has a
// length) and the bit depth (16-bit, 24-bit, 32-bit float; 24 at first), to a
// WAV file. OK says so with exportChosen(range, bitDepth); the window then
// asks where (Session.exportProblem first) and exports.
Dialog {
    id: dialog

    property var ranges: []
    property int rangeIndex: 0
    property int bitDepthIndex: 0
    readonly property string range: ranges.length > rangeIndex ? ranges[rangeIndex].value : "arrangement"
    readonly property int bitDepth: Session.exportBitDepthChoices[bitDepthIndex].value

    signal exportChosen(string range, int bitDepth)

    function defaultBitDepthIndex() {
        const choices = Session.exportBitDepthChoices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === Session.defaultExportBitDepth)
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
    onAccepted: exportChosen(range, bitDepth)

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
        Label { text: qsTr("Bit Depth") }
        ChoiceBox {
            objectName: "exportBitDepth"
            Layout.fillWidth: true
            choices: Session.exportBitDepthChoices
            chosenIndex: dialog.bitDepthIndex
            onChosen: index => dialog.bitDepthIndex = index
        }
        Label { text: qsTr("File Type") }
        Label { text: "WAV" }
    }
}
