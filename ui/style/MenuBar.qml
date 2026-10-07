import QtQuick
import QtQuick.Templates as T
import SUBstation

// The menus, in the window's title bar (TitleBar.qml), on its colour.
T.MenuBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    delegate: MenuBarItem {}

    contentItem: Row {
        spacing: control.spacing
        Repeater {
            model: control.contentModel
        }
    }

    background: Item {
        implicitHeight: 24
    }
}
