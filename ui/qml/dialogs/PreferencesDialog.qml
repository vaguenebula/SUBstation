import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// Options › Preferences… (Ctrl+,, or a click on the audio device's name in the
// transport bar): the Audio, MIDI, Plug-ins and Look and Feel pages, and Close. Changes apply
// at once (see AudioPage). The Audio page follows the device while the dialog
// shows (Session.audioPreferences open() and close()).
Dialog {
    id: dialog

    property alias currentPage: tabs.currentIndex
    readonly property alias audioPage: audioPage
    readonly property alias midiPage: midiPage
    readonly property alias pluginsPage: pluginsPage
    readonly property alias lookAndFeelPage: lookAndFeelPage

    objectName: "preferencesDialog"
    title: qsTr("Preferences")
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.Close
    width: Math.max(500, implicitWidth)

    onAboutToShow: {
        Session.audioPreferences.open()
        Session.midiPreferences.open()
    }
    onClosed: Session.audioPreferences.close()

    contentItem: ColumnLayout {
        spacing: 0

        TabBar {
            id: tabs
            objectName: "preferencesTabs"

            TabButton { text: qsTr("Audio") }
            TabButton { text: qsTr("MIDI") }
            TabButton { text: qsTr("Plug-ins") }
            TabButton { text: qsTr("Look and Feel") }
        }

        // QTabWidget's pane: a BORDER line round the page.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitWidth: pages.implicitWidth + 2 * 10
            implicitHeight: Math.max(300, pages.implicitHeight + 2 * 10)
            color: "transparent"
            border.color: Theme.border

            StackLayout {
                id: pages
                anchors.fill: parent
                anchors.margins: 10
                currentIndex: tabs.currentIndex

                AudioPage {
                    id: audioPage
                    // Hardware Setup may run a message loop of its own: the dialog waits for it.
                    onControlPanelShowing: showing => dialog.enabled = !showing
                }
                MidiPage {
                    id: midiPage
                }
                PluginsPage {
                    id: pluginsPage
                }
                LookAndFeelPage {
                    id: lookAndFeelPage
                }
            }
        }
    }
}
