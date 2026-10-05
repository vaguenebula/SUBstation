import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// The clip view: what a double-clicked clip opens. A MIDI clip gets the piano
// roll and nothing else. Audio clips get their controls on the left (Warp,
// Pitch, Mix) and large waveforms on the right; with several open, the controls
// edit all of them at once (ClipViewController has the rules).
//
// Give it the clips to show:
//
//   ClipView {
//       trackId: track            // the track of the ids in clipIds
//       clipIds: [clip]           // or [{trackId: t, clipId: c}, ...] for clips on several tracks
//       leadClipId: clip          // optional: the clip double-clicked (a MIDI one opens alone)
//       onCloseRequested: ...     // Esc, ×, or its clips are gone: back to the devices
//       onLocateRequested: beat => ...  // the piano roll's ruler was clicked: play from this
//                                         // arrangement beat (MainWindow.locate: the insert marker
//                                         // there, and the bridge located)
//       onStatusMessage: message => window.showMessage(message)
//   }
//
// They show top track first, then by time. Esc (or ×) asks to be closed.
// Shown, it opens them afresh (a MIDI clip fitted to the view); hidden, it
// forgets them (a key sounding stops). When it is shown or given clips, the
// notes (MIDI) or the view (audio, for Esc) take the keyboard; the notes take
// Delete, Ctrl+A, Ctrl+D and Ctrl+U before the window's shortcuts while they
// have it.
FocusScope {
    id: view

    property string trackId
    property var clipIds: []
    property string leadClipId

    readonly property alias controller: clipController
    readonly property alias pianoRollView: rollView
    // Showing a MIDI clip in the piano roll (else audio clips, or nothing).
    readonly property bool midi: clipController.midi

    signal closeRequested()
    signal locateRequested(real beat)
    signal statusMessage(string message)

    // The notes (MIDI) or the view (audio) take the keyboard.
    function focusContent() {
        if (clipController.midi)
            rollView.focusNotes()
        else
            view.forceActiveFocus()
    }

    implicitWidth: 900
    implicitHeight: 360

    ClipViewController {
        id: clipController
        session: Session
        pianoRoll: rollView.roll
        trackId: view.trackId
        clipIds: view.clipIds
        leadClipId: view.leadClipId
        active: view.visible
        onCloseRequested: view.closeRequested()
        onStatusMessage: message => view.statusMessage(message)
        onOpened: if (view.visible) view.focusContent()
    }

    Keys.onEscapePressed: view.closeRequested()

    Rectangle {
        anchors.fill: parent
        color: Theme.panel
    }

    // --- Header: colour, name, info, close ---

    Item {
        id: header
        width: parent.width
        height: 30

        Rectangle {
            width: 5
            height: header.height - 1
            color: clipController.color
            visible: clipController.count > 0
        }

        Row {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Label {
                objectName: "clipName"
                anchors.verticalCenter: parent.verticalCenter
                text: clipController.name
                font: Theme.uiFont(10, true)
            }
            Label {
                objectName: "clipInfo"
                anchors.verticalCenter: parent.verticalCenter
                text: clipController.info
                color: Theme.textDim
            }
        }

        RoleButton {
            objectName: "closeButton"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            role: "flat"
            text: "×"
            font.pointSize: 14
            tooltip: qsTr("Back to the arrangement (Esc)")
            onClicked: view.closeRequested()
        }

        Rectangle {
            y: header.height - 1
            width: parent.width
            height: 1
            color: Theme.border
        }
    }

    Item {
        id: body
        y: header.height
        width: parent.width
        height: parent.height - header.height

        // --- Audio clips: controls and waveforms ---

        Item {
            id: audioPage
            objectName: "audioPage"
            anchors.fill: parent
            visible: clipController.count > 0 && !clipController.midi

            Flickable {
                id: controls
                width: 260
                height: parent.height
                contentWidth: width
                contentHeight: column.y + column.implicitHeight + 8
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                Column {
                    id: column
                    x: 8
                    y: 8
                    width: controls.width - 16
                    spacing: 6

                    ClipViewSection {
                        width: column.width
                        title: qsTr("Warp")

                        // The old grid: Warp; the mode across; "Seg. BPM" and its box; :2 and ×2
                        // under the box. Columns as QGridLayout sized them: each its widest
                        // control (the caption, the 34 px buttons) plus a third of what is left.
                        Item {
                            id: warpGrid

                            readonly property real extra: Math.max(0, width - 12 - bpmCaption.implicitWidth - 68)
                            readonly property real first: bpmCaption.implicitWidth + extra / 3
                            readonly property real other: 34 + extra / 3

                            width: parent.width
                            height: halveBpm.y + halveBpm.height

                            ToggleButton {
                                objectName: "warp"
                                width: warpGrid.first
                                height: 20
                                role: "activator"
                                text: qsTr("Warp")
                                tooltip: qsTr("Lock the clip to the beat grid, so it follows the project tempo")
                                checked: clipController.warp
                                onToggled: clipController.setWarp(checked)
                            }

                            ComboBox {
                                id: warpMode
                                objectName: "warpMode"
                                y: 24
                                width: parent.width
                                focusPolicy: Qt.NoFocus
                                model: clipController.warpModes
                                currentIndex: clipController.warpModeIndex
                                displayText: currentIndex < 0 ? qsTr("Mixed") : currentText
                                onActivated: index => clipController.setWarpMode(index)
                                ToolTip.visible: hovered && !popup.visible
                                ToolTip.text: qsTr("How the clip is stretched (and transposed)")
                                ToolTip.delay: 700

                                delegate: ItemDelegate {
                                    required property var modelData
                                    required property int index
                                    width: ListView.view ? ListView.view.width : implicitWidth
                                    text: modelData
                                    highlighted: warpMode.highlightedIndex === index
                                    hoverEnabled: true
                                    ToolTip.visible: hovered
                                    ToolTip.text: clipController.warpModeTips[index]
                                    ToolTip.delay: 700
                                }
                            }

                            Label {
                                id: bpmCaption
                                y: segmentBpm.y
                                width: warpGrid.first
                                height: segmentBpm.height
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("Seg. BPM")
                                color: Theme.textDim
                                font: Theme.uiFont(8)
                            }
                            ValueBox {
                                id: segmentBpm
                                objectName: "segmentBpm"
                                x: warpGrid.first + 6
                                y: warpMode.y + warpMode.height + 4
                                width: parent.width - x
                                height: 20
                                from: 20
                                to: 999
                                step: 0.01
                                decimals: 2
                                sampleText: "999.00"
                                formatter: v => v.toFixed(2)
                                value: clipController.segmentBpm
                                onMoved: (value, gestureKey) => clipController.setSegmentBpm(value, gestureKey)
                                ToolTip.visible: hovered && !dragging
                                ToolTip.text: qsTr("Segment BPM: the tempo of the audio in the clip. Warped clips play at\nproject tempo ÷ segment BPM speed.")
                                ToolTip.delay: 700
                            }

                            RoleButton {
                                id: halveBpm
                                objectName: "halveBpm"
                                x: segmentBpm.x
                                y: segmentBpm.y + segmentBpm.height + 4
                                width: warpGrid.other
                                height: 20
                                topPadding: 0
                                bottomPadding: 0
                                text: ":2"
                                tooltip: qsTr("Halve each clip's segment BPM (warped clips play twice as fast)")
                                onClicked: clipController.scaleBpm(0.5)
                            }
                            RoleButton {
                                objectName: "doubleBpm"
                                x: halveBpm.x + halveBpm.width + 6
                                y: halveBpm.y
                                width: parent.width - x
                                height: 20
                                topPadding: 0
                                bottomPadding: 0
                                text: "×2"
                                tooltip: qsTr("Double each clip's segment BPM (warped clips play half as fast)")
                                onClicked: clipController.scaleBpm(2.0)
                            }
                        }
                    }

                    ClipViewSection {
                        width: column.width
                        title: qsTr("Pitch")

                        RowLayout {
                            width: parent.width

                            ClipViewKnob {
                                objectName: "transpose"
                                Layout.fillWidth: true
                                controller: clipController
                                knob: "transpose"
                                active: !clipController.repitch
                            }
                            ClipViewKnob {
                                objectName: "detune"
                                Layout.fillWidth: true
                                controller: clipController
                                knob: "detune"
                                active: !clipController.repitch
                            }
                        }
                    }

                    ClipViewSection {
                        width: column.width
                        title: qsTr("Mix")

                        RowLayout {
                            width: parent.width

                            ClipViewKnob {
                                objectName: "gain"
                                Layout.fillWidth: true
                                controller: clipController
                                knob: "gain"
                            }
                            ClipViewKnob {
                                objectName: "pan"
                                Layout.fillWidth: true
                                controller: clipController
                                knob: "pan"
                            }
                        }
                    }

                    Label {
                        width: column.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Warped clips follow the project tempo. Transpose keeps the speed.")
                        color: Theme.textDisabled
                    }
                }
            }

            ClipWaveform {
                objectName: "clipWaveform"
                x: controls.width
                width: parent.width - controls.width
                height: parent.height
                session: Session
                controller: clipController
            }
        }

        // --- A MIDI clip: the piano roll ---

        PianoRollView {
            id: rollView
            objectName: "pianoRollView"
            anchors.fill: parent
            visible: clipController.count > 0 && clipController.midi
            onLocateRequested: beat => view.locateRequested(beat)
        }
    }
}
