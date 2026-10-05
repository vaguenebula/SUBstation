import QtQuick
import QtQuick.Controls
import SUBstation

// A combo box over a list of choices as the app layer hands them out:
// [{label, value[, enabled, toolTip]}] (a choice with enabled false is greyed
// out, its tooltip saying why). It shows `chosenIndex` (what the model has) and
// says which one the user picked with chosen(index); it shows the model's
// choice again afterwards, whatever the model made of it. It never takes the
// keyboard focus (Space stays play/stop), as the old combo boxes didn't.
ComboBox {
    id: box

    property var choices: []
    property int chosenIndex: -1
    // The tooltip of the box itself.
    property string tooltip: ""

    signal chosen(int index)

    model: choices
    textRole: "label"
    currentIndex: chosenIndex
    focusPolicy: Qt.NoFocus

    onActivated: index => {
        box.chosen(index)
        box.currentIndex = Qt.binding(() => box.chosenIndex)
    }

    delegate: ItemDelegate {
        id: row

        required property var modelData
        required property int index

        width: ListView.view ? ListView.view.width : implicitWidth
        text: modelData.label
        enabled: modelData.enabled !== false
        highlighted: box.highlightedIndex === index

        ToolTip.visible: hovered && (modelData.toolTip || "") !== ""
        ToolTip.text: modelData.toolTip || ""
        ToolTip.delay: 500
    }

    HoverHandler {
        id: hover
    }
    ToolTip.visible: box.tooltip !== "" && hover.hovered && !box.popup.visible
    ToolTip.text: box.tooltip
    ToolTip.delay: 700
}
