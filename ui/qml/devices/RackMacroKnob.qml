import QtQuick
import QtQuick.Controls
import SUBstation

// One of a rack's macros: its name ("Macro N", or as named) over its knob
// (0..1), the name lit while something is mapped to it; the knob's tooltip
// lists what it moves, and its dot shows its automation (red while it plays,
// grey while overridden), which it follows as it plays. Turning it sets what it
// moves (one undo step per drag); pressing it shows its automation in the
// arrangement. Double-click the name to rename it in place (Enter, Escape or
// leaving the field keeps the name typed; an empty one names it by its number
// again); right-click for its menu (the panel's).
Item {
    id: cell

    property string trackId
    property string rackId
    property int macroIndex: 0
    required property var panel
    readonly property RackMacro macro: rackMacro
    readonly property alias knob: knob
    readonly property alias nameLabel: name
    readonly property alias renameField: renameField
    readonly property bool renaming: renameField.visible

    implicitWidth: 60  // MACRO_WIDTH
    implicitHeight: column.implicitHeight

    // Rename it in place.
    function startRename() {
        if (renameField.visible)
            return
        renameField.text = rackMacro.name
        renameField.visible = true
        renameField.selectAll()
        renameField.forceActiveFocus()
    }

    function finishRename() {
        if (!renameField.visible)
            return
        const typed = renameField.text
        renameField.visible = false
        rackMacro.rename(typed)  // ("": by its number again)
    }

    RackMacro {
        id: rackMacro
        session: Session
        trackId: cell.trackId
        rackId: cell.rackId
        index: cell.macroIndex
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onPressed: mouse => cell.panel.showMacroMenu(cell, cell, mouse.x, mouse.y)
    }

    Column {
        id: column
        width: cell.implicitWidth
        spacing: 1

        Text {
            id: name
            objectName: "macroName"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: rackMacro.name
            color: rackMacro.mapped ? Theme.text : Theme.textDim
            font: Theme.uiFont(7)

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: cell.startRename()
            }
        }
        Knob {
            id: knob
            objectName: "knob"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 30  // MACRO_KNOB
            height: 30
            from: 0
            to: 1
            defaultValue: 0
            value: rackMacro.value
            automation: rackMacro.automation
            formatter: v => Math.round(v * 100) + " %"
            onMoved: (v, key) => rackMacro.set(v, key)
            onTouched: rackMacro.touch()
            ToolTip.text: rackMacro.toolTip
        }
    }

    TextField {
        id: renameField
        objectName: "renameField"
        visible: false
        x: -4
        y: name.y - 2
        width: cell.width + 8
        height: name.height + 4
        padding: 0
        horizontalAlignment: TextInput.AlignHCenter
        font: Theme.uiFont(7)
        onEditingFinished: cell.finishRename()
        onActiveFocusChanged: if (!activeFocus) cell.finishRename()
        Keys.onEscapePressed: cell.finishRename()
    }
}
