import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// Preferences › Audio (Session.audioPreferences, the old PreferencesDialog's
// form): the driver type (ASIO greyed out in a build without it, its tooltip
// saying so), the device and its Hardware Setup (ASIO), the outputs the master
// plays on (ASIO), the sample rate, the buffer size (greyed out when the
// driver sets it), exclusive mode (WASAPI) and the audio threads; below them
// what runs, or why it didn't open. Each choice opens the device with it at
// once, since what a device offers is only known while it is open; the lists
// then show what it offers.
ColumnLayout {
    id: page

    readonly property var prefs: Session.audioPreferences

    // Hardware Setup is showing (true) or done (false).
    signal controlPanelShowing(bool showing)

    function showControlPanel() {
        controlPanelShowing(true)
        prefs.showControlPanel()
        controlPanelShowing(false)
    }

    spacing: 10

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 12
        rowSpacing: 6

        Label { text: qsTr("Driver Type") }
        ChoiceBox {
            objectName: "driver"
            Layout.fillWidth: true
            choices: page.prefs.driverChoices
            chosenIndex: page.prefs.driverIndex
            onChosen: index => page.prefs.chooseDriver(index)
        }

        Label { text: qsTr("Audio Device") }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ChoiceBox {
                objectName: "device"
                Layout.fillWidth: true
                enabled: page.prefs.deviceEnabled
                choices: page.prefs.deviceChoices
                chosenIndex: page.prefs.deviceIndex
                onChosen: index => page.prefs.chooseDevice(index)
            }
            RoleButton {
                objectName: "controlPanel"
                visible: page.prefs.controlPanelVisible
                enabled: page.prefs.controlPanelEnabled
                text: qsTr("Hardware Setup")
                tooltip: qsTr("The driver's own settings (buffer size, clock, routing...)")
                onClicked: page.showControlPanel()
            }
        }

        Label {
            visible: page.prefs.outputsVisible
            text: qsTr("Output Channels")
        }
        ChoiceBox {
            objectName: "outputs"
            Layout.fillWidth: true
            visible: page.prefs.outputsVisible
            tooltip: qsTr("The outputs the master plays on")
            choices: page.prefs.outputChoices
            chosenIndex: page.prefs.outputIndex
            onChosen: index => page.prefs.chooseOutputs(index)
        }

        Label { text: qsTr("Sample Rate") }
        ChoiceBox {
            objectName: "sampleRate"
            Layout.fillWidth: true
            choices: page.prefs.sampleRateChoices
            chosenIndex: page.prefs.sampleRateIndex
            onChosen: index => page.prefs.chooseSampleRate(index)
        }

        Label { text: qsTr("Buffer Size") }
        ChoiceBox {
            objectName: "bufferSize"
            Layout.fillWidth: true
            enabled: page.prefs.bufferEnabled
            tooltip: page.prefs.bufferToolTip
            choices: page.prefs.bufferChoices
            chosenIndex: page.prefs.bufferIndex
            onChosen: index => page.prefs.chooseBufferSize(index)
        }

        Item {
            visible: page.prefs.exclusiveVisible
            implicitWidth: 1
            implicitHeight: 1
        }
        CheckBox {
            id: exclusive
            objectName: "exclusive"
            visible: page.prefs.exclusiveVisible
            focusPolicy: Qt.NoFocus
            text: qsTr("Exclusive mode (lower latency; other apps are silenced)")
            checked: page.prefs.exclusive
            onToggled: {
                page.prefs.setExclusive(checked)
                checked = Qt.binding(() => page.prefs.exclusive)
            }
        }

        Label { text: qsTr("Audio Threads") }
        ChoiceBox {
            objectName: "threads"
            Layout.fillWidth: true
            tooltip: qsTr("Tracks render on this many threads at once (the audio thread and its helpers). "
                          + "1 renders every track on the audio thread.")
            choices: page.prefs.threadChoices
            chosenIndex: page.prefs.threadIndex
            onChosen: index => page.prefs.chooseThreads(index)
        }

        Item {
            implicitWidth: 1
            implicitHeight: 1
        }
        CheckBox {
            id: backgroundFreezing
            objectName: "backgroundFreezing"
            focusPolicy: Qt.NoFocus
            text: qsTr("Background freezing (unchanged tracks play from a cache)")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Tracks that haven't changed for a while play from a RAM cache instead of "
                               + "running their plug-ins, saving CPU on repeated passes.")
            checked: page.prefs.backgroundFreezing
            onToggled: {
                page.prefs.setBackgroundFreezing(checked)
                checked = Qt.binding(() => page.prefs.backgroundFreezing)
            }
        }
    }

    Label {
        objectName: "audioStatus"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        text: page.prefs.status
        textFormat: Text.RichText
        wrapMode: Text.Wrap
        color: Theme.textDim
    }

    Item {
        Layout.fillHeight: true
    }
}
