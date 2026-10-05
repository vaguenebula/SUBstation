import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// The bar on top of the window (transport_bar.py), left to right: the tempo,
// the time signature, the metronome, the project key and the computer MIDI
// keyboard | the position (bar. beat. sixteenth), play, stop, record, the
// count-in, Re-Enable Automation and Lock Envelopes, the oscilloscope | loop,
// follow, the CPU load and the audio device (a click opens Preferences).
//
// Its controls show the project, the bridge and the session, and call them:
// Play's and Record's checked state follows the bridge, never the click; the
// toggles show what the model has (a click asks it to change). Every box,
// button and the scope are Theme.controlHeight (28 px) tall; none takes the
// keyboard focus, so Space stays play/stop.
Rectangle {
    id: bar

    // The arrangement view (its `follow`, read and written by the Follow button).
    property Item arrangement: null
    readonly property alias transportState: transport

    // The device's name was clicked.
    signal preferencesRequested()

    objectName: "transportBar"
    implicitHeight: 40
    color: Theme.panel

    TransportState {
        id: transport
        session: Session
    }

    // The line along its bottom.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    component Separator: Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: Theme.controlHeight
        Layout.leftMargin: 2
        Layout.rightMargin: 2
        color: Theme.border
    }

    component DimLabel: Label {
        color: Theme.textDim
        verticalAlignment: Text.AlignVCenter
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 5
        anchors.bottomMargin: 6
        spacing: 5

        ValueBox {
            id: tempo
            objectName: "tempo"
            Layout.preferredHeight: Theme.controlHeight
            from: 20
            to: 999
            step: 0.25
            decimals: 2
            sampleText: "999.00"
            value: Session.project.tempo
            onMoved: (value, gestureKey) => Session.editor.setTempo(value, gestureKey)

            ToolTip.visible: hovered && !dragging
            ToolTip.text: qsTr("Tempo (drag, Shift for fine steps, double-click to type)")
            ToolTip.delay: 700
        }
        DimLabel { text: qsTr("BPM") }
        Separator {}
        ValueBox {
            objectName: "numerator"
            Layout.preferredHeight: Theme.controlHeight
            from: 1
            to: 32
            step: 0.1
            decimals: 0
            sampleText: "32"
            formatter: value => String(Math.round(value))
            value: transport.numerator
            onMoved: value => Session.editor.setTimeSignature(Math.round(value), transport.denominator)
        }
        Label { text: "/" }
        ValueBox {
            objectName: "denominator"
            Layout.preferredHeight: Theme.controlHeight
            from: 1
            to: 32
            decimals: 0
            choices: transport.denominators
            sampleText: "32"
            formatter: value => String(Math.round(value))
            value: transport.denominator
            onMoved: value => Session.editor.setTimeSignature(transport.numerator, Math.round(value))
        }
        Separator {}
        ToggleButton {
            objectName: "metronome"
            Layout.preferredHeight: Theme.controlHeight
            role: "tool"
            iconName: "metronome"
            tooltip: qsTr("Metronome")
            checkable: false
            checked: Session.bridge.metronome
            onClicked: Session.bridge.metronome = !Session.bridge.metronome
        }
        // The project's key: audio added with a key in its file name is transposed to it.
        ChoiceBox {
            objectName: "key"
            Layout.preferredHeight: Theme.controlHeight
            choices: transport.keys
            chosenIndex: transport.keyIndex
            tooltip: qsTr("Project key. Audio files with a key in their name (\"Loop_128_Am\")\n"
                          + "are transposed to it when added; a tempo in the name warps them to it.")
            onChosen: index => Session.editor.setKeyByName(transport.keys[index].name)
        }
        ToggleButton {
            objectName: "computerKeys"
            Layout.preferredHeight: Theme.controlHeight
            role: "tool"
            text: "⌨"
            tooltip: Session.computerKeyboard.toolTip
            checkable: false
            checked: Session.computerKeyboard.enabled
            onClicked: Session.computerKeyboard.toggle()
        }

        Item {
            Layout.fillWidth: true
        }

        Rectangle {
            id: position
            objectName: "position"
            readonly property string text: transport.positionText
            Layout.preferredHeight: Theme.controlHeight
            Layout.preferredWidth: Math.max(96, positionText.implicitWidth + 2 * 7)
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius

            Text {
                id: positionText
                anchors.centerIn: parent
                text: transport.positionText
                font: Theme.uiFont(11, true)
                color: Theme.text
            }
        }
        ToggleButton {
            objectName: "play"
            Layout.preferredHeight: Theme.controlHeight
            role: "play"
            iconName: "play"
            tooltip: qsTr("Play / Stop (Space)")
            checkable: false
            checked: Session.bridge.playing
            onClicked: Session.togglePlay()
        }
        IconButton {
            objectName: "stop"
            Layout.preferredHeight: Theme.controlHeight
            iconName: "stop"
            tooltip: qsTr("Stop (press again to return to the start)")
            onClicked: Session.stop()
        }
        ToggleButton {
            objectName: "record"
            Layout.preferredHeight: Theme.controlHeight
            role: "record"
            iconName: "record"
            tooltip: qsTr("Arrangement Record (F9): records the armed tracks")
            checkable: false
            checked: Session.bridge.recording
            onClicked: Session.toggleRecord()
        }
        ChoiceBox {
            id: countIn
            objectName: "countIn"
            Layout.preferredHeight: Theme.controlHeight
            choices: Session.countInChoices
            chosenIndex: {
                const choices = Session.countInChoices
                for (let i = 0; i < choices.length; ++i)
                    if (choices[i].value === Session.countInBars)
                        return i
                return 0
            }
            tooltip: qsTr("Count-in: the metronome counts in this long before recording starts")
            onChosen: index => Session.countInBars = Session.countInChoices[index].value
        }
        // Lit while automation is overridden (a target changed by hand): a click brings it back.
        ToggleButton {
            objectName: "reEnable"
            Layout.preferredHeight: Theme.controlHeight
            role: "re-enable"
            iconName: "re_enable_automation"
            tooltip: qsTr("Re-Enable Automation")
            checkable: false
            enabled: Session.automationOverridden
            checked: Session.automationOverridden
            onClicked: Session.bridge.reEnableAutomation()
        }
        ToggleButton {
            objectName: "lockEnvelopes"
            Layout.preferredHeight: Theme.controlHeight
            role: "tool"
            iconName: "lock_envelopes"
            tooltip: qsTr("Lock Envelopes: automation stays in place when clips move (off: it moves with them)")
            checkable: false
            checked: Session.project.automationLocked
            onClicked: Session.editor.setAutomationLocked(!Session.project.automationLocked)
        }
        Item {
            implicitWidth: 1
        }
        Oscilloscope {
            objectName: "scope"
            Layout.preferredHeight: Theme.controlHeight
            Layout.preferredWidth: implicitWidth
            feed: Session.bridge
        }

        Item {
            Layout.fillWidth: true
        }

        ToggleButton {
            objectName: "loop"
            Layout.preferredHeight: Theme.controlHeight
            role: "tool"
            iconName: "loop"
            tooltip: qsTr("Loop (Ctrl+L)")
            checkable: false
            checked: Session.project.loopEnabled
            onClicked: Session.editor.setLoopEnabled(!Session.project.loopEnabled)
        }
        ToggleButton {
            objectName: "follow"
            readonly property bool available: bar.arrangement !== null && bar.arrangement.follow !== undefined
            Layout.preferredHeight: Theme.controlHeight
            role: "tool"
            iconName: "follow"
            tooltip: qsTr("Follow the playhead")
            checkable: false
            checked: available && bar.arrangement.follow === true
            onClicked: if (available) bar.arrangement.follow = !bar.arrangement.follow
        }
        Separator {}
        DimLabel {
            objectName: "cpu"
            Layout.minimumWidth: 60
            text: transport.cpuText
        }
        DimLabel {
            objectName: "device"
            text: transport.deviceText

            MouseArea {
                id: deviceArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton
                onPressed: bar.preferencesRequested()
            }
            ToolTip.visible: deviceArea.containsMouse
            ToolTip.text: transport.deviceToolTip
            ToolTip.delay: 700
        }
    }
}
