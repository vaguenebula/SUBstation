import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// A render in the background (exporting, freezing, reversing long clips): its
// progress, with Cancel (rendering.py's RenderProgress). It shows while
// Session.render is active, and it is modal: the window goes on meanwhile (it
// repaints, its meters move) but takes no edits, its shortcuts included,
// since the render is of the project as it was when it started.
//
// Cancel (the button, or Esc) only asks the render to stop: the label says
// "Cancelling…" and Cancel is disabled until it has, then the dialog goes.
// Cancel never takes the keyboard focus: Space, meant for the transport,
// doesn't cancel.
Dialog {
    id: dialog

    readonly property var progress: Session.render

    objectName: "renderDialog"
    title: progress.title
    visible: progress.active
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    closePolicy: Popup.NoAutoClose
    width: Math.max(380, implicitWidth)

    // Its window takes the keys again afterwards (its shortcuts work).
    onClosed: if (Window.window) Window.window.requestActivate()

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            objectName: "renderLabel"
            Layout.fillWidth: true
            text: dialog.progress.label  // ("Cancelling…" once cancelled)
            elide: Text.ElideRight
        }
        ProgressBar {
            objectName: "renderBar"
            Layout.fillWidth: true
            from: 0
            to: 1
            value: dialog.progress.progress
            indeterminate: dialog.progress.busy
        }

        // Esc cancels (the window's other shortcuts are blocked by the dialog meanwhile).
        Shortcut {
            sequences: [StandardKey.Cancel]
            enabled: dialog.visible
            onActivated: dialog.progress.cancel()
        }
    }

    footer: Rectangle {
        implicitHeight: cancel.implicitHeight + 24
        color: Theme.window

        Button {
            id: cancel
            objectName: "renderCancel"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: Math.max(80, implicitContentWidth + leftPadding + rightPadding)
            text: qsTr("Cancel")
            focusPolicy: Qt.NoFocus
            enabled: !dialog.progress.cancelled
            onClicked: dialog.progress.cancel()
        }
    }
}
