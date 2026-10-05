import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// A message box, as QMessageBox showed them: its title, an icon, the text
// (rich text allowed) and a row of buttons at the bottom right. Give it the
// buttons as `choices` ([{text, value}], left to right); answered(value) says
// which was pressed (Return: the `defaultValue`'s, Esc: `escapeValue`). It is
// modal: the window takes no input meanwhile.
//
//   MessageBox {
//       text: qsTr("Delete it?")
//       icon: "question"
//       choices: [{text: qsTr("Yes"), value: "yes"}, {text: qsTr("No"), value: "no"}]
//       escapeValue: "no"
//       onAnswered: value => ...
//   }
Dialog {
    id: box

    property string text
    // "information", "warning", "question", "about" (the application's icon), or "".
    property string icon: "information"
    property var choices: [{text: qsTr("OK"), value: "ok"}]
    property string defaultValue: choices.length > 0 ? choices[0].value : ""
    property string escapeValue: choices.length > 0 ? choices[choices.length - 1].value : ""

    signal answered(string value)

    // Shows it with this text (and title).
    function show(text, title) {
        box.text = text
        if (title !== undefined)
            box.title = title
        open()
    }

    function answer(value) {
        close()
        answered(value)
    }

    title: "SUBstation"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    width: Math.min(Math.max(implicitWidth, 320), parent ? parent.width - 40 : 560)

    contentItem: FocusScope {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        focus: true

        // (Shortcuts, not keys: a button clicked before may have the focus.)
        Shortcut {
            sequences: [StandardKey.Cancel]
            enabled: box.visible
            onActivated: box.answer(box.escapeValue)
        }
        Shortcut {
            sequences: ["Return", "Enter"]
            enabled: box.visible
            onActivated: box.answer(box.defaultValue)
        }

        RowLayout {
            id: row
            anchors.fill: parent
            spacing: 14

            Item {
                Layout.alignment: Qt.AlignTop
                visible: box.icon !== ""
                implicitWidth: 40
                implicitHeight: 40

                Icon {
                    anchors.centerIn: parent
                    visible: box.icon === "about"
                    name: "app_icon"
                    size: 40
                }
                Rectangle {
                    anchors.centerIn: parent
                    visible: box.icon !== "about"
                    width: 32
                    height: 32
                    radius: 16
                    color: box.icon === "warning" ? Theme.accent : Theme.surface
                    border.color: Theme.border

                    Text {
                        anchors.centerIn: parent
                        text: box.icon === "question" ? "?" : (box.icon === "warning" ? "!" : "i")
                        font: Theme.uiFont(14, true)
                        color: box.icon === "warning" ? Theme.accentText : Theme.text
                    }
                }
            }
            Label {
                id: message
                objectName: "messageText"
                Layout.fillWidth: true
                Layout.maximumWidth: 480
                text: box.text
                textFormat: Text.AutoText
                wrapMode: Text.Wrap
                onLinkActivated: link => Qt.openUrlExternally(link)
            }
        }
    }

    footer: Rectangle {
        implicitHeight: buttons.implicitHeight + 24
        implicitWidth: buttons.implicitWidth + 24
        color: Theme.window

        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Repeater {
                model: box.choices

                Button {
                    required property var modelData
                    objectName: "choice_" + modelData.value
                    text: modelData.text
                    implicitWidth: Math.max(80, implicitContentWidth + leftPadding + rightPadding)
                    onClicked: box.answer(modelData.value)
                }
            }
        }
    }
}
