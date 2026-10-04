import QtQuick
import QtQuick.Controls
import SUBstation

// The Ableton-style number box (ValueBoxItem, from value_box.py), with the text
// field that double-clicking (without a default) or typing a digit (with one)
// opens over it, applied on Return or when it loses the focus.
//
//   ValueBox {
//       from: 20; to: 999; step: 0.25; decimals: 2; sampleText: "999.00"
//       value: project.tempo
//       onMoved: (value, gestureKey) => editor.setTempo(value, gestureKey)
//   }
ValueBoxItem {
    id: box

    onEditRequested: (initialText, selectAll) => editor.openWith(initialText, selectAll)

    Popup {
        id: editor

        property bool done: true

        function openWith(initialText, selectAll) {
            field.text = initialText
            done = false
            open()
            field.forceActiveFocus()
            if (selectAll)
                field.selectAll()
            else
                field.cursorPosition = field.length
        }

        function finish() {
            if (done)
                return
            done = true
            box.applyTyped(field.text)
        }

        width: box.width
        height: box.height
        padding: 0
        focus: true
        closePolicy: Popup.CloseOnPressOutside
        onClosed: finish()

        contentItem: TextField {
            id: field
            padding: 0
            font: box.font
            horizontalAlignment: TextInput.AlignHCenter
            onAccepted: editor.close()
        }
        background: null
    }
}
