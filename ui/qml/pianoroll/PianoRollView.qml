import QtQuick
import QtQuick.Controls
import SUBstation

// The piano roll, laid out like Ableton's MIDI editor: the ruler on top, the
// keys on the left, the notes in the middle with the song's chords along their
// top (while the harmony shows: C) and the note tools floating over them, the
// velocities below, and scroll bars. The headphones button turns hearing notes
// off; the bend button (B) shows the notes' bend curves to edit (MIDI 2.0's
// per-note pitch bend), with the bend bar at the grid's top right. The clips
// it shows are the roll's (`roll.setClips(refs)`, or
// `roll.setClip(trackId, clipId)`; the clip view sets them).
//
//   ┌──────────┬────────────────────────────┬───┐
//   │ hear bend│ PianoRuler (24 px)         │   │
//   ├──────────┼────────────────────────────┼───┤
//   │ PianoKeys│ NoteGrid, ChordLane on top │ v │
//   │ (64 px)  │             ┌───────────┐  │ b │
//   │          │             │ NoteTools │  │ a │
//   │          │             └───────────┘  │ r │
//   ├──────────┼────────────────────────────┼───┤
//   │ Velocity │ VelocityLane (72 px)       │   │
//   ├──────────┼────────────────────────────┤   │
//   │          │ hbar                       │   │
//   └──────────┴────────────────────────────┴───┘
Item {
    id: view

    readonly property PianoRoll roll: pianoRoll
    readonly property NoteGrid grid: noteGrid
    readonly property alias keys: pianoKeys
    readonly property alias ruler: pianoRuler
    readonly property alias velocity: velocityLane
    readonly property alias tools: noteTools
    readonly property alias chords: chordLane
    readonly property alias preview: previewButton
    readonly property alias bend: bendButton
    readonly property alias bendTools: bendTools

    readonly property int keysWidth: 64
    readonly property int rulerHeight: 24
    readonly property int velocityHeight: 72
    readonly property int barWidth: Theme.scrollBarWidth

    // Play from this arrangement beat (the ruler was clicked).
    signal locateRequested(real beat)

    // Focusing the piano roll focuses the notes.
    function focusNotes() {
        noteGrid.forceActiveFocus()
    }

    PianoRoll {
        id: pianoRoll
        session: Session
        onLocateRequested: beat => view.locateRequested(beat)
    }

    Row {
        x: Math.round((view.keysWidth - width) / 2)
        y: Math.round((view.rulerHeight - height) / 2)
        spacing: 2

        ToggleButton {
            id: previewButton
            objectName: "preview"
            role: "tool"
            iconName: "headphones"
            tooltip: qsTr("Hear notes as you click, add and move them")
            checked: pianoRoll.preview
            onToggled: pianoRoll.preview = checked
        }
        ToggleButton {
            id: bendButton
            objectName: "bendMode"
            role: "tool"
            iconName: "bend"
            tooltip: qsTr("Edit the notes' pitch bends: curves with points, vibrato and slides (B);\nhold Shift there for the notes")
            checked: pianoRoll.bendMode
            onToggled: {
                pianoRoll.bendMode = checked
                checked = Qt.binding(() => pianoRoll.bendMode)
            }
        }
    }

    PianoRuler {
        id: pianoRuler
        objectName: "pianoRuler"
        x: view.keysWidth
        width: noteGrid.width
        height: view.rulerHeight
        clip: true
        session: Session
        roll: pianoRoll

        RollPlayhead {
            anchors.fill: parent
            roll: pianoRoll
            ruler: true
        }
    }

    PianoKeys {
        id: pianoKeys
        objectName: "pianoKeys"
        y: view.rulerHeight
        width: view.keysWidth
        height: noteGrid.height
        clip: true
        session: Session
        roll: pianoRoll
    }

    NoteGrid {
        id: noteGrid
        objectName: "noteGrid"
        x: view.keysWidth
        y: view.rulerHeight
        width: Math.max(0, view.width - view.keysWidth - view.barWidth)
        height: Math.max(0, view.height - view.rulerHeight - view.velocityHeight - view.barWidth)
        clip: true
        session: Session
        roll: pianoRoll

        ChordLane {
            id: chordLane
            objectName: "chordLane"
            width: parent.width
            height: 18
            visible: Session.harmony.shown
            roll: pianoRoll
        }

        RollPlayhead {
            anchors.fill: parent
            roll: pianoRoll
        }

        NoteTools {
            id: noteTools
            objectName: "noteTools"
            roll: pianoRoll
        }

        BendTools {
            id: bendTools
            objectName: "bendTools"
            roll: pianoRoll
        }
    }

    // The scroll bars follow the roll, except while dragged: then the roll follows them.
    ScrollBar {
        id: vbar
        objectName: "vbar"
        x: noteGrid.x + noteGrid.width
        y: noteGrid.y
        width: view.barWidth
        height: noteGrid.height
        orientation: Qt.Vertical
        policy: ScrollBar.AlwaysOn
        focusPolicy: Qt.NoFocus
        size: pianoRoll.vScrollPage / Math.max(1, pianoRoll.vScrollTotal)
        stepSize: pianoRoll.rowHeight / Math.max(1, pianoRoll.vScrollTotal)
        onPositionChanged: if (pressed) pianoRoll.scrollToY(position * pianoRoll.vScrollTotal)

        Binding on position {
            when: !vbar.pressed
            value: pianoRoll.vScrollValue / Math.max(1, pianoRoll.vScrollTotal)
        }
    }

    Rectangle {
        y: noteGrid.y + noteGrid.height
        width: view.keysWidth
        height: view.velocityHeight
        color: Theme.panel

        Label {
            anchors.centerIn: parent
            text: qsTr("Velocity")
            color: Theme.textDim
            font: Theme.uiFont(8)
        }
    }

    VelocityLane {
        id: velocityLane
        objectName: "velocityLane"
        x: view.keysWidth
        y: noteGrid.y + noteGrid.height
        width: noteGrid.width
        height: view.velocityHeight
        clip: true
        session: Session
        roll: pianoRoll

        RollPlayhead {
            anchors.fill: parent
            roll: pianoRoll
        }
    }

    ScrollBar {
        id: hbar
        objectName: "hbar"
        x: view.keysWidth
        y: velocityLane.y + velocityLane.height
        width: noteGrid.width
        height: view.barWidth
        orientation: Qt.Horizontal
        policy: ScrollBar.AlwaysOn
        focusPolicy: Qt.NoFocus
        size: pianoRoll.hScrollPage / Math.max(1, pianoRoll.hScrollTotal)
        stepSize: Math.max(1, Math.floor(noteGrid.width / 20)) / Math.max(1, pianoRoll.hScrollTotal)
        onPositionChanged: if (pressed) pianoRoll.scrollToX(position * pianoRoll.hScrollTotal)

        Binding on position {
            when: !hbar.pressed
            value: pianoRoll.hScrollValue / Math.max(1, pianoRoll.hScrollTotal)
        }
    }
}
