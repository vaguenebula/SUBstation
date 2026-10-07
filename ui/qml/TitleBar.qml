pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// The main window's title bar, with its menus: on Windows it is the window's
// only one (WindowFrame takes the system's caption away). Left to right: the
// application's icon and the menus (what is put in it: a MenuBar); the title,
// in the middle of the window; the status line (`message`) and a project's
// plug-ins loading; minimize, maximize and close (only while WindowFrame is
// active: elsewhere the system's title bar has them).
//
// Its background, the title and the status line drag the window; a
// double-click there maximizes it, a right-click opens its system menu (while
// one of the menus is open, a click there closes it instead). The top edge
// resizes the window. The maximize button is the system's (the snap layouts
// show over it): it is drawn here as WindowFrame says it is hovered or pressed.
Rectangle {
    id: bar

    // The menus.
    default property alias menus: menuSlot.data
    property alias title: titleText.text
    // The status line's text ("": none).
    property alias message: statusText.text
    readonly property alias frame: windowFrame

    readonly property bool windowActive: bar.Window.active
    readonly property bool maximized: bar.Window.window !== null && bar.Window.window.visibility === Window.Maximized
    // Windows' caption glyphs (Windows 11's font, else Windows 10's).
    readonly property string glyphFont: Qt.fontFamilies().indexOf("Segoe Fluent Icons") >= 0 ? "Segoe Fluent Icons"
                                                                                               : "Segoe MDL2 Assets"
    // Where the right side (status, plug-ins loading, the window's buttons) starts.
    readonly property real rightEdge: windowFrame.active ? captionButtons.x : bar.width

    objectName: "titleBar"
    implicitHeight: 32
    color: Theme.panel

    WindowFrame {
        id: windowFrame
        objectName: "windowFrame"
        window: bar.Window.window
        titleBar: bar
        maximizeButton: maximizeCaption
        overlay: bar.Overlay.overlay
    }

    // The line along its bottom.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    // A window button: Windows' size and glyphs, in the theme's colours (close: red under the mouse).
    component CaptionButton: Rectangle {
        id: button

        property string glyph
        property bool closeButton: false
        // Whether its own mouse area handles it (the maximize button's is WindowFrame).
        property bool handlesMouse: true
        property bool hovered: mouseArea.containsMouse
        property bool pressed: mouseArea.pressed

        signal clicked()

        width: 46
        height: bar.height - 1
        color: !button.hovered ? "transparent"
             : button.closeButton ? (button.pressed ? "#a32a1d" : "#c42b1c")
             : button.pressed ? Theme.panelAlt : Theme.surface

        Text {
            anchors.centerIn: parent
            text: button.glyph
            font.family: bar.glyphFont
            font.pixelSize: 10
            color: button.closeButton && button.hovered ? "#ffffff" : bar.windowActive ? Theme.text : Theme.textDim
        }

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            visible: button.handlesMouse
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    Row {
        id: leading
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1
        spacing: 4

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "app_icon"
            size: 16
        }

        Item {
            id: menuSlot
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // In the middle of the window, unless the menus or the right side want its place.
    Text {
        id: titleText
        objectName: "windowTitle"

        readonly property real leftBound: leading.x + leading.width + 16
        readonly property real rightBound: bar.rightEdge - 16

        x: Math.max(leftBound, Math.min((bar.width - implicitWidth) / 2, rightBound - implicitWidth))
        width: Math.max(0, Math.min(implicitWidth, rightBound - x))
        anchors.verticalCenter: parent.verticalCenter
        color: bar.windowActive ? Theme.text : Theme.textDim
        font: Theme.font
        elide: Text.ElideRight
    }

    RowLayout {
        anchors.left: titleText.right
        anchors.leftMargin: 24
        anchors.right: windowFrame.active ? captionButtons.left : parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Text {
            id: statusText
            objectName: "statusText"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            color: Theme.textDim
            font: Theme.font
            elide: Text.ElideRight
        }

        RowLayout {
            id: pluginsLoading
            objectName: "pluginsLoading"
            visible: Session.pluginsTotal > 0
            spacing: 6

            Text {
                objectName: "pluginsLabel"
                text: Session.pluginsLoadingText
                color: Theme.textDim
                font: Theme.font
            }
            ProgressBar {
                objectName: "pluginsBar"
                Layout.preferredWidth: 120
                Layout.preferredHeight: 10
                from: 0
                to: Math.max(1, Session.pluginsTotal)
                value: Session.pluginsLoaded
            }
        }
    }

    Row {
        id: captionButtons
        objectName: "captionButtons"
        anchors.right: parent.right
        anchors.top: parent.top
        visible: windowFrame.active

        CaptionButton {
            objectName: "minimizeButton"
            glyph: ""
            onClicked: bar.Window.window.showMinimized()
        }
        CaptionButton {
            id: maximizeCaption
            objectName: "maximizeButton"
            glyph: bar.maximized ? "" : ""
            handlesMouse: false
            hovered: windowFrame.maximizeHovered
            pressed: windowFrame.maximizePressed
        }
        CaptionButton {
            objectName: "closeButton"
            glyph: ""
            closeButton: true
            onClicked: bar.Window.window.close()
        }
    }
}
