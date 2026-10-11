import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// Preferences › Look and Feel: the theme (Theme.names: Default, Disableton,
// Flashbang, Gay). A theme is colours only; it applies at once, everywhere,
// and is kept for the next run (Theme.name).
ColumnLayout {
    id: page

    spacing: 10

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 12
        rowSpacing: 6

        Label { text: qsTr("Theme") }
        ChoiceBox {
            objectName: "theme"
            Layout.fillWidth: true
            choices: Theme.names.map(name => ({ label: name, value: name }))
            chosenIndex: Theme.names.indexOf(Theme.name)
            onChosen: index => Theme.name = Theme.names[index]
        }
    }

    Item { Layout.fillHeight: true }
}
