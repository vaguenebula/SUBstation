import QtQuick
import QtQuick.Templates as T
import SUBstation

// QMenu: PANEL_ALT, a BORDER line, 3 px inside it; as wide as its widest item.
T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 0
    padding: 4
    overlap: 4

    delegate: MenuItem {}

    contentItem: ListView {
        implicitHeight: contentHeight
        implicitWidth: {
            let widest = 0
            for (let i = 0; i < control.count; ++i) {
                const item = control.itemAt(i)
                if (item)
                    widest = Math.max(widest, item.implicitWidth)
            }
            return widest
        }
        model: control.contentModel
        interactive: Window.window ? contentHeight + control.topPadding + control.bottomPadding > Window.window.height
                                   : false
        clip: true
        currentIndex: control.currentIndex
        T.ScrollIndicator.vertical: ScrollIndicator {}
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 24
        color: Theme.panelAlt
        border.color: Theme.border
    }
}
