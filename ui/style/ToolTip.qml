import QtQuick
import QtQuick.Templates as T
import SUBstation

// QToolTip: PANEL_ALT with a BORDER line, 3 px of padding; below what it is about.
// While the main window's info view shows (Hints), a tip of an item in that
// window (not in a popup) is said there instead, at once: it opens without
// its delay, draws nothing and takes no room.
T.ToolTip {
    id: control

    readonly property bool inInfoView: Hints.routes(parent)

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: parent ? parent.height + 4 : 0

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 6
    padding: inInfoView ? 0 : 4
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    // (The attached ToolTip sets its delay just before it opens the tip.)
    onDelayChanged: if (inInfoView && delay !== 0) delay = 0
    onOpened: if (inInfoView) Hints.show(control, text)
    onTextChanged: if (opened && inInfoView) Hints.show(control, text)
    onInInfoViewChanged: {
        if (!inInfoView)
            Hints.hide(control)
        else if (opened)
            Hints.show(control, text)
    }
    onClosed: Hints.hide(control)

    contentItem: Text {
        text: control.inInfoView ? "" : control.text
        font: control.font
        color: Theme.text
    }

    background: Rectangle {
        visible: !control.inInfoView
        color: Theme.panelAlt
        border.color: Theme.border
    }
}
