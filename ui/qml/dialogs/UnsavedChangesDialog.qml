import QtQuick
import SUBstation

// "Save changes to the current project?" (Save, Discard, Cancel), asked before
// New, Open, Open Recent and Quit while the project has unsaved changes
// (MainWindow._confirm_discard). answered(value): "save", "discard" or
// "cancel" (Esc too).
MessageBox {
    objectName: "unsavedChangesDialog"
    text: Session.confirmDiscardText
    icon: "question"
    choices: [
        {text: qsTr("Save"), value: "save"},
        {text: qsTr("Discard"), value: "discard"},
        {text: qsTr("Cancel"), value: "cancel"}
    ]
    defaultValue: "save"
    escapeValue: "cancel"
}
