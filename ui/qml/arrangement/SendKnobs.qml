import QtQuick
import QtQuick.Controls
import SUBstation

// A strip's send knobs (mixer_controls.py's SendControls), one per return,
// side by side, each with the return's letter (in the accent colour while it
// taps before the fader). A knob moves as a volume fader does; typing a dB
// value works. Right-click: Pre-Fader, Remove Send, Show Automation (not on
// a knob greyed out: the return feeds this strip). Those that don't fit don't show.
Item {
    id: sendKnobs

    required property TrackHeaderItem header
    required property ArrangementMenu menu

    readonly property int count: header.sends.length
    readonly property int slotWidth: 36  // a knob and its letter, at most
    readonly property int slot: count ? Math.max(24, Math.min(slotWidth, Math.floor(width / count))) : 0
    readonly property int knobSize: Math.max(16, Math.min(height, slot - 12))

    Repeater {
        model: sendKnobs.header.sends

        Item {
            id: send

            required property var modelData
            required property int index
            readonly property string returnId: modelData.returnId

            x: index * sendKnobs.slot
            width: sendKnobs.slot
            height: sendKnobs.height
            visible: x + sendKnobs.slot <= sendKnobs.width

            Text {
                objectName: "sendLetter"
                width: sendKnobs.slot - sendKnobs.knobSize - 1
                height: parent.height
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                text: send.modelData.letter
                font: Theme.uiFont(8, true)
                color: send.modelData.preFader ? Theme.accent : Theme.textDim
            }

            Knob {
                id: knob
                objectName: "sendKnob"
                x: sendKnobs.slot - sendKnobs.knobSize
                y: Math.floor((parent.height - sendKnobs.knobSize) / 2)
                width: sendKnobs.knobSize
                height: sendKnobs.knobSize
                from: 0
                to: 1
                defaultValue: 0
                wheel: false
                enabled: send.modelData.enabled
                value: send.modelData.value
                automation: send.modelData.automation
                formatter: v => send.modelData.toolTip
                parser: text => sendKnobs.header.parseSendLevel(text)
                onMoved: (value, gestureKey) => sendKnobs.header.setSend(send.returnId, value, gestureKey)
                onTouched: sendKnobs.header.touchSend(send.returnId)
            }

            MouseArea {
                anchors.fill: knob
                acceptedButtons: Qt.RightButton
                onPressed: sendKnobs.menu.show(sendKnobs.header.sendMenuEntries(send.returnId), sendKnobs.header, knob,
                                               mouseX, mouseY)
            }
        }
    }
}
