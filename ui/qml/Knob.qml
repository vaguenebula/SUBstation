import QtQuick
import QtQuick.Controls
import SUBstation

// The rotary knob (KnobItem): its value as its tooltip, and the small text
// field typing a digit opens when it has a `parser`: centred on the knob, at
// least 48 px wide, applied on Return or when it loses the focus.
//
//   Knob {
//       from: -1; to: 1; defaultValue: 0; bipolar: true; wheel: false
//       value: model.pan
//       formatter: v => formatPan(v)
//       parser: text => parsePan(text)       // a number, or null
//       onMoved: (value, gestureKey) => editor.setTrackParam(id, "pan", value, gestureKey)
//       onTouched: editor.touchParameter(id, "pan")
//   }
KnobItem {
    id: knob

    HoverHandler {
        id: hover
    }

    ToolTip.visible: hover.hovered && !dragging && !editor.visible && text !== ""
    ToolTip.text: text
    ToolTip.delay: 700

    onEditRequested: initialText => editor.openWith(initialText)

    Popup {
        id: editor

        property bool done: true

        function openWith(initialText) {
            field.text = initialText
            done = false
            open()
            field.forceActiveFocus()
            field.cursorPosition = field.length
        }

        function finish() {
            if (done)
                return
            done = true
            knob.applyTyped(field.text)
        }

        width: Math.max(48, metrics.advanceWidth + 16)
        height: 20
        x: (knob.width - width) / 2
        y: knob.height / 2 - 10
        padding: 0
        focus: true
        closePolicy: Popup.CloseOnPressOutside
        onClosed: finish()

        TextMetrics {
            id: metrics
            font: field.font
            text: "100%"
        }

        contentItem: TextField {
            id: field
            padding: 0
            horizontalAlignment: TextInput.AlignHCenter
            onAccepted: editor.close()
        }
        background: null
    }
}
